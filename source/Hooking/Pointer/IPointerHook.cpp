#include "IPointerHook.h"

#include <fcntl.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <android/log.h>

#ifndef kANDROID_LOG_TAG
#define kANDROID_LOG_TAG "PtrHook"
#endif

#define PH_LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO,  kANDROID_LOG_TAG, __VA_ARGS__))
#define PH_LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, kANDROID_LOG_TAG, __VA_ARGS__))

namespace {

// Per-hook trampoline layout (32 B):
//   [+00] bti c                ; 0xD503245F
//   [+04] ldr x17, [pc, #+12]  ; load IPointerHook* (literal @ +16)
//   [+08] ldr x16, [pc, #+16]  ; load glue addr    (literal @ +24)
//   [+12] br  x16              ; 0xD61F0200
//   [+16] .quad hook_ptr
//   [+24] .quad glue_addr
// kTrampolineSize must agree with ph_trampoline_pool.s `.rept` block.
constexpr size_t kTrampolineSize = 32;

inline void EncodeTrampoline(void* rw_addr, IPointerHook* hook) {
    uint32_t* code = reinterpret_cast<uint32_t*>(rw_addr);
    code[0] = 0xD503245Fu;  // bti c
    code[1] = 0x58000071u;  // ldr x17, [pc, #+12]
    code[2] = 0x58000090u;  // ldr x16, [pc, #+16]
    code[3] = 0xD61F0200u;  // br  x16

    uint64_t* data = reinterpret_cast<uint64_t*>(code + 4);
    data[0] = reinterpret_cast<uint64_t>(hook);
    data[1] = reinterpret_cast<uint64_t>(&_ph_glue_entry);
}

// Unified /proc/self/mem read/write backend.
#ifndef PH_CACHE_SELFMEM_FD
#define PH_CACHE_SELFMEM_FD 0
#endif

class SelfMemFd {
public:
    SelfMemFd() {
#if PH_CACHE_SELFMEM_FD
        fd_ = AcquireCached();
#else
        fd_ = OpenOnce();
#endif
    }
    ~SelfMemFd() {
#if !PH_CACHE_SELFMEM_FD
        if (fd_ >= 0) close(fd_);
#endif
    }
    SelfMemFd(const SelfMemFd&) = delete;
    SelfMemFd& operator=(const SelfMemFd&) = delete;
    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

private:
    static int OpenOnce() {
        int orig_dumpable = prctl(PR_GET_DUMPABLE);
        bool toggled = false;
        if (orig_dumpable != 1) {
            if (prctl(PR_SET_DUMPABLE, 1) == 0) toggled = true;
        }

        int fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
        int saved_errno = errno;

        if (toggled && orig_dumpable >= 0) {
            if (prctl(PR_SET_DUMPABLE, orig_dumpable) != 0) {
                PH_LOGE("PR_SET_DUMPABLE restore to %d failed: %s",
                    orig_dumpable, strerror(errno));
            }
        }

        errno = saved_errno;
        if (fd < 0) {
            PH_LOGE("open(/proc/self/mem) failed: %s", strerror(errno));
        }
        return fd;
    }
#if PH_CACHE_SELFMEM_FD
    static int AcquireCached() {
        static std::atomic<int> cached_fd{-1};
        int fd = cached_fd.load(std::memory_order_acquire);
        if (fd >= 0) return fd;
        fd = OpenOnce();
        if (fd < 0) return -1;
        int expected = -1;
        if (!cached_fd.compare_exchange_strong(expected, fd,
                std::memory_order_acq_rel)) {
            close(fd);   // Lost the race; another thread cached its fd first.
            return expected;
        }
        return fd;
    }
#endif

    int fd_;
};

inline bool SelfMemRead(uintptr_t address, void* buffer, size_t len) {
    SelfMemFd fd;
    if (!fd) return false;
    size_t total = 0;
    while (total < len) {
        ssize_t n = pread(fd.get(), static_cast<uint8_t*>(buffer) + total,
                          len - total, static_cast<off_t>(address + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            PH_LOGE("SelfMemRead: pread(self/mem, %p) failed: %s",
                (void*)address, strerror(errno));
            return false;
        }
        if (n == 0) {
            PH_LOGE("SelfMemRead: pread(self/mem, %p) short read",
                (void*)address);
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

inline bool SelfMemWrite(uintptr_t address, const void* data, size_t len) {
    SelfMemFd fd;
    if (!fd) return false;
    size_t total = 0;
    while (total < len) {
        ssize_t n = pwrite(fd.get(), static_cast<const uint8_t*>(data) + total,
                           len - total, static_cast<off_t>(address + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            PH_LOGE("SelfMemWrite: pwrite(self/mem, %p) failed: %s",
                (void*)address, strerror(errno));
            return false;
        }
        if (n == 0) {
            PH_LOGE("SelfMemWrite: pwrite(self/mem, %p) short write",
                (void*)address);
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

// Linker-reserved RX region in libdfmhook .text (see ph_trampoline_pool.s).
// n × 32 B slots, brk-prefilled, bumped monotonically, never recycled.

extern "C" {
    extern uint8_t __ph_trampoline_pool_start[];
    extern uint8_t __ph_trampoline_pool_end[];
}

class InBinaryTrampolinePool {
public:
    static InBinaryTrampolinePool& Instance() {
        static InBinaryTrampolinePool inst;
        return inst;
    }

    uintptr_t Alloc(IPointerHook* hook) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(__ph_trampoline_pool_start);
        const uintptr_t end  = reinterpret_cast<uintptr_t>(__ph_trampoline_pool_end);
        const size_t capacity = (end - base) / kTrampolineSize;

        size_t slot;
        {
            std::lock_guard<std::mutex> g(slot_mtx_);
            if (next_slot_ >= capacity) {
                PH_LOGE("[InBinaryTrampolinePool] exhausted (max %zu slots)", capacity);
                return 0;
            }
            slot = next_slot_++;
        }

        uintptr_t tramp = base + slot * kTrampolineSize;
        if (!WriteSlotSealed(tramp, hook)) {
            // Slot is wasted (next_slot_ already incremented) but that's
            // cheap — n slots, never recycled by design.
            return 0;
        }
        return tramp;
    }

private:
    InBinaryTrampolinePool() = default;

    bool WriteSlotSealed(uintptr_t tramp, IPointerHook* hook) {
        std::lock_guard<std::mutex> guard(write_mtx_);

        alignas(uint32_t) uint8_t buf[kTrampolineSize];
        EncodeTrampoline(buf, hook);

        return SelfMemWrite(tramp, buf, kTrampolineSize);
    }

    std::mutex slot_mtx_;
    std::mutex write_mtx_;
    size_t     next_slot_ = 0;
};

// ─── SelfTest ─────────────────────────────────────────────────────────────

constexpr uint32_t kExpectedGluePrefix[4] = {
    0xD50324DFu,  // bti jc
    0xD10C43FFu,  // sub sp, sp, #0x310
    0xF90187F1u,  // str x17, [sp, #0x308]
    0xD53B4210u,  // mrs x16, nzcv
};

bool CheckGluePrefix() {
    const uint32_t* code = reinterpret_cast<const uint32_t*>(&_ph_glue_entry);
    for (int i = 0; i < 4; ++i) {
        if (code[i] != kExpectedGluePrefix[i]) {
            PH_LOGE("[SelfTest] glue prefix mismatch at +%d: got %08x want %08x",
                 i * 4, code[i], kExpectedGluePrefix[i]);
            return false;
        }
    }
    return true;
}

bool CheckEncodeTrampoline() {
    alignas(16) uint8_t buf[kTrampolineSize] = {0};
    constexpr uintptr_t kSentinelHook = 0xC0FFEEC0FFEEC0FFULL;
    EncodeTrampoline(buf, reinterpret_cast<IPointerHook*>(kSentinelHook));

    const uint32_t* code = reinterpret_cast<const uint32_t*>(buf);
    const uint64_t* data = reinterpret_cast<const uint64_t*>(buf + 16);

    if (code[0] != 0xD503245Fu ||
        code[1] != 0x58000071u ||
        code[2] != 0x58000090u ||
        code[3] != 0xD61F0200u)
    {
        PH_LOGE("[SelfTest] EncodeTrampoline instr mismatch: %08x %08x %08x %08x",
             code[0], code[1], code[2], code[3]);
        return false;
    }
    if (data[0] != kSentinelHook) {
        PH_LOGE("[SelfTest] EncodeTrampoline hook literal mismatch: %llx vs %llx",
             data[0], kSentinelHook);
        return false;
    }
    if (data[1] != reinterpret_cast<uint64_t>(&_ph_glue_entry)) {
        PH_LOGE("[SelfTest] EncodeTrampoline glue literal mismatch: %llx vs %llx",
             data[1], reinterpret_cast<uint64_t>(&_ph_glue_entry));
        return false;
    }
    return true;
}

using SelfTestFn = void (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t, uint64_t,
                            double, double, double, double);

// `noinline` + asm constraints force every arg through the real calling
// convention — that's the codegen we're verifying.
[[gnu::noinline]] void SelfTestTarget(
    uint64_t a, uint64_t b, uint64_t c, uint64_t d,
    uint64_t e, uint64_t f, uint64_t g, uint64_t h,
    double da, double db, double dc, double dd)
{
    __asm__ volatile(""
        :
        : "r"(a), "r"(b), "r"(c), "r"(d),
          "r"(e), "r"(f), "r"(g), "r"(h),
          "w"(da), "w"(db), "w"(dc), "w"(dd)
        : "memory");
}

SelfTestFn volatile g_self_test_fn = &SelfTestTarget;

class SelfTestHook : public IPointerHook {
public:
    bool                     fired = false;
    std::vector<std::string> errors;

    std::string GetName() const override { return "SelfTest"; }
    uintptr_t   GetElfBase() const override { return 0; }
    uintptr_t   GetSlotAddr() const override {
        return reinterpret_cast<uintptr_t>(&g_self_test_fn);
    }
    uintptr_t   GetTargetAddr() const override { return 0; }

    uintptr_t OnCall(RegContext* ctx) override {
        fired = true;

        constexpr uint64_t kPattern = 0x1111111111111111ULL;
        char buf[128];
        for (int i = 0; i < 8; ++i) {
            uint64_t expected = kPattern * (i + 1);
            if (ctx->general.x[i] != expected) {
                snprintf(buf, sizeof(buf), "x%d: got %#018llx want %#018llx",
                    i, ctx->general.x[i], expected);
                errors.emplace_back(buf);
            }
        }
        for (int i = 0; i < 4; ++i) {
            double expected = (i + 1) * 1.5;
            if (ctx->floating.d[i] != expected) {
                snprintf(buf, sizeof(buf), "d%d: got %g want %g",
                    i, ctx->floating.d[i], expected);
                errors.emplace_back(buf);
            }
        }
        if (ctx->_pad != 0) {
            snprintf(buf, sizeof(buf), "_pad: got %#llx want 0", ctx->_pad);
            errors.emplace_back(buf);
        }

        return GetOrigAddr();
    }
};

bool CheckEndToEnd() {
    SelfTestHook hook;
    hook.Resolve();
    hook.Install();

    constexpr uint64_t kPattern = 0x1111111111111111ULL;
    g_self_test_fn(
        kPattern * 1, kPattern * 2, kPattern * 3, kPattern * 4,
        kPattern * 5, kPattern * 6, kPattern * 7, kPattern * 8,
        1.5, 3.0, 4.5, 6.0);

    hook.Restore();

    if (!hook.fired) {
        PH_LOGE("[SelfTest] OnCall did not fire — dispatch path is broken");
        return false;
    }
    if (!hook.errors.empty()) {
        for (const auto& e : hook.errors) {
            PH_LOGE("[SelfTest] ctx mismatch: %s", e.c_str());
        }
        return false;
    }
    return true;
}

std::atomic<bool> g_self_test_passed{false};

} // anonymous namespace

#define MAKE_CRASH()     \
    __asm__ volatile (   \
        "mov x0, xzr;"   \
        "mov x29, x0;"   \
        "mov sp, x0;"    \
        "br x0;"         \
        : : :            \
    );

// asm/C++ bridge
extern "C" uintptr_t _ph_dispatcher(RegContext* ctx, IPointerHook* hook) {
    return hook->OnCall(ctx);
}

std::string RegContext::ToString() const
{
    std::string result;
    char line[64];
    for (int i = 0; i < 29; i++) {
        snprintf(line, sizeof(line), "x%d: %#016llx\n",
            i, general.x[i]);
        result += line;
    }
    snprintf(line, sizeof(line), "fp: %#016llx\n",   fp);   result += line;
    snprintf(line, sizeof(line), "lr: %#016llx\n",   lr);   result += line;
    snprintf(line, sizeof(line), "sp: %#016llx\n",   sp);   result += line;
    snprintf(line, sizeof(line), "nzcv: %#016llx\n", nzcv); result += line;
    return result;
}

IPointerHook::IPointerHook() = default;

IPointerHook::~IPointerHook()
{
    // Residual UAF: an in-flight caller that LDR'd trampoline_ before this
    // restore will still reach OnCall on a partly-destroyed derived object.
    Restore();
}

void IPointerHook::Resolve()
{
    slot_ = GetSlotAddr();

    if (uintptr_t target = GetTargetAddr(); target) {
        orig_ = target;
    } else {
        uintptr_t temp = 0;
        if (SelfMemRead(slot_, &temp, sizeof(uintptr_t)) && temp != 0) {
            orig_ = StripPAC(temp);
        } else {
            PH_LOGE("[%s] Resolve failed: orig is null", GetName().c_str());
            slot_ = 0;
            return;
        }
    }

    PH_LOGI("[%s] Resolved slot=%p orig=%p",
         GetName().c_str(), (void*)slot_, (void*)orig_);
}

uintptr_t IPointerHook::AllocTrampoline()
{
    return InBinaryTrampolinePool::Instance().Alloc(this);
}

bool IPointerHook::PrepareTrampoline()
{
    if (trampoline_ != 0) return true;
    if (slot_ == 0) {
        PH_LOGE("[%s] PrepareTrampoline: Resolve() not called yet", GetName().c_str());
        return false;
    }

    uintptr_t tramp = AllocTrampoline();
    if (tramp == 0) {
        PH_LOGE("[%s] PrepareTrampoline: AllocTrampoline returned 0", GetName().c_str());
        return false;
    }

    trampoline_ = tramp;
    PH_LOGI("[%s] trampoline @ %p", GetName().c_str(), (void*)trampoline_);
    return true;
}

void IPointerHook::Install()
{
    if (!PrepareTrampoline()) return;

    if (!SelfMemWrite(slot_, &trampoline_, sizeof(uintptr_t))) {
        PH_LOGE("[%s] Install failed: SelfMemWrite error at %p",
             GetName().c_str(), (void*)slot_);
        return;
    }

    installed_ = true;

    PH_LOGI("[%s] Install: slot=%p orig=%p tramp=%p",
        GetName().c_str(), (void*)slot_, (void*)orig_, (void*)trampoline_);
}

void IPointerHook::Restore()
{
    if (!installed_) return;

    if (!SelfMemWrite(slot_, &orig_, sizeof(uintptr_t))) {
        PH_LOGE("[%s] Restore failed: SelfMemWrite error at %p",
             GetName().c_str(), (void*)slot_);
    }
    installed_ = false;
    PH_LOGI("[%s] Restore", GetName().c_str());
}

bool IPointerHook::SelfTest()
{
    bool ok = CheckGluePrefix()
           && CheckEncodeTrampoline()
           && CheckEndToEnd();
    g_self_test_passed.store(ok, std::memory_order_release);
    if (ok) {
        PH_LOGI("[SelfTest] all checks passed");
    } else {
        PH_LOGE("[SelfTest] FAILED — PointerHookManager will refuse Adds");
    }
    return ok;
}

bool IPointerHook::PassedSelfTest()
{
    return g_self_test_passed.load(std::memory_order_acquire);
}

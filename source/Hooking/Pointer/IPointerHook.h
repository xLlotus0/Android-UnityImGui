#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

struct RegContext;

#ifdef __cplusplus
extern "C" {
#endif

// asm/C++ ABI boundary. See ph_arm64_glue.s for the register-save contract.
void          _ph_glue_entry();

// CallOrigWithContext backends — shared body, two symbols so the C++ ABI
// picks the return register (x0 vs d0) from the declared return type.
std::uint64_t _ph_call_orig_with_ctx_i(RegContext* ctx, std::uintptr_t func,
                                       std::size_t spill_bytes);
double        _ph_call_orig_with_ctx_d(RegContext* ctx, std::uintptr_t func,
                                       std::size_t spill_bytes);

#ifdef __cplusplus
}
#endif

struct RegContext {
    union FPReg {
        __int128_t q;
        struct {
            double d1;
            double d2;
        } d;
        struct {
            float f1;
            float f2;
            float f3;
            float f4;
        } f;
    };
    static_assert(sizeof(FPReg) == 16, "FPReg size mismatch");

    struct SRegView {
        FPReg data[32];
        float&       operator[](size_t n)       { return data[n].f.f1; }
        const float& operator[](size_t n) const { return data[n].f.f1; }
    };

    struct DRegView {
        FPReg data[32];
        double&       operator[](size_t n)       { return data[n].d.d1; }
        const double& operator[](size_t n) const { return data[n].d.d1; }
    };

    union {
        SRegView s;  // s[n] == q[n].f.f1 == Arm64 sN
        DRegView d;  // d[n] == q[n].d.d1 == Arm64 dN
        FPReg q[32];
        struct {
        FPReg q0, q1, q2, q3, q4, q5, q6, q7;
        FPReg q8, q9, q10, q11, q12, q13, q14, q15, q16, q17, q18, q19, q20, q21, q22, q23, q24, q25, q26, q27, q28, q29,
            q30, q31;
        } regs;
    } floating;

    union {
        uint64_t x[29];
        struct {
        uint64_t x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18, x19, x20, x21, x22,
            x23, x24, x25, x26, x27, x28;
        } regs;
    } general;

    uint64_t fp;
    uint64_t lr;
    uint64_t sp;

    uint64_t nzcv;

    // Transient hook-ptr stash for the glue prolog; cleared before OnCall.
    uint64_t _pad;

    std::string ToString() const;
};
static_assert(sizeof(RegContext) == 0x310, "Wrong size on RegContext");
static_assert(offsetof(RegContext, floating) == 0x0, "Wrong offset on floating");
static_assert(offsetof(RegContext, general) == 0x200, "Wrong offset on general");
static_assert(offsetof(RegContext, fp) == 0x2E8, "Wrong offset on fp");
static_assert(offsetof(RegContext, lr) == 0x2F0, "Wrong offset on lr");
static_assert(offsetof(RegContext, sp) == 0x2F8, "Wrong offset on sp");
static_assert(offsetof(RegContext, nzcv) == 0x300, "Wrong offset on nzcv");
static_assert(offsetof(RegContext, _pad) == 0x308, "Wrong offset on _pad");

class IPointerHook
{
public:
    IPointerHook();
    virtual ~IPointerHook();

    virtual std::string GetName() const = 0;
    virtual uintptr_t   GetElfBase() const = 0;
    virtual uintptr_t   GetSlotAddr() const = 0;
    virtual uintptr_t   GetTargetAddr() const = 0;

    // Return 0 to RET to the caller, or any address to tail-br it.
    virtual uintptr_t   OnCall(RegContext* ctx) = 0;

    virtual void Resolve();
    virtual void Install();
    virtual void Restore();

    uintptr_t GetOrigAddr() const { return orig_; }
    uintptr_t GetTrampolineAddr() const { return trampoline_; }

    template <typename Ret, typename... Args>
    Ret CallOrig(Args&&... args)
    {
        using fn_t = Ret (*)(std::decay_t<Args>...);
        return reinterpret_cast<fn_t>(orig_)(std::forward<Args>(args)...);
    }

    template <typename Ret>
    Ret CallOrigWithContext(RegContext* ctx, std::size_t spill_bytes = 0)
    {
        if constexpr (std::is_void_v<Ret>) {
            (void)_ph_call_orig_with_ctx_i(ctx, orig_, spill_bytes);
        } else if constexpr (std::is_floating_point_v<Ret>) {
            return static_cast<Ret>(_ph_call_orig_with_ctx_d(ctx, orig_, spill_bytes));
        } else if constexpr (std::is_pointer_v<Ret>) {
            return reinterpret_cast<Ret>(_ph_call_orig_with_ctx_i(ctx, orig_, spill_bytes));
        } else {
            static_assert(sizeof(Ret) <= sizeof(std::uint64_t),
                        "CallOrigWithContext: Ret >8 bytes; set ctx->general.x[8] "
                        "to a buffer ptr and ignore the return value.");
            return static_cast<Ret>(_ph_call_orig_with_ctx_i(ctx, orig_, spill_bytes));
        }
    }

    static uintptr_t StripPAC(uintptr_t p)
    {
        static const uintptr_t kMask = []() -> uintptr_t {
            uintptr_t self = reinterpret_cast<uintptr_t>(&_ph_glue_entry);
            return (self >> 39) ? 0x0000FFFFFFFFFFFFULL : 0x0000007FFFFFFFFFULL;
        }();
        return p & kMask;
    }

    // Validates glue prefix + EncodeTrampoline + a full dispatch round-trip
    // against a local sentinel. Manager refuses Install until this passes.
    static bool SelfTest();
    static bool PassedSelfTest();

protected:
    virtual uintptr_t AllocTrampoline();

private:
    bool PrepareTrampoline();

    uintptr_t slot_       = 0;
    uintptr_t orig_       = 0;
    uintptr_t trampoline_ = 0;
    bool      installed_  = false;
};

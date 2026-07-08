#pragma once

#include "KittyMemoryEx/KittyMemoryEx.hpp"
#include "KittyMemoryEx/KittyMemoryMgr.hpp"
#include "KittyMemoryEx/KittyScanner.hpp"

#include <android/log.h>
#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <string>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>
#include <sys/cdefs.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifndef kANDROID_LOG_TAG
#define kANDROID_LOG_TAG "KittyEx"
#endif
#define KT_LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO,  kANDROID_LOG_TAG, __VA_ARGS__))
#define KT_LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, kANDROID_LOG_TAG, __VA_ARGS__))

namespace KT
{

// 清高位 tag/PAC 签名位，保留低 39 位 VA
inline uintptr_t Strip(uintptr_t address) {
    return address & (uintptr_t)0x7fffffffff;
}

// 校验地址在合理 VA 区间，高位仅接受 0 或 Android tagged-heap (0xB4..)
// 注意：ARM64 PAC 下函数指针/栈上 LR 高位是签名码（如走 backtrace 进 libc 时遇到的情形）
//      先 Strip 再 IsValid，否则会被误判 !valid
inline bool IsValid(uintptr_t address) {
    if (!address || address < 0x10000000)
        return false;
    uintptr_t highBits = address & ~(uintptr_t)0x7fffffffff;
    return highBits == 0 || highBits == (uintptr_t)0xB400000000000000;
}

template<class T>
inline bool IsValid(const T& c) {
    return IsValid((uintptr_t)c);
}

enum class MemError : uint8_t {
    None = 0,
    InvalidAddress,
    ReadFailed,
    WriteFailed,
    SyscallFailed,
    IOWriteFailed,
    OpenFailed,
    NotInitialized,
};

inline thread_local MemError t_lastError = MemError::None;

inline void ClearError() { t_lastError = MemError::None; }
inline void SetError(MemError e) { t_lastError = e; }
inline MemError LastError() { auto e = t_lastError; t_lastError = MemError::None; return e; }
inline bool HasError() { return t_lastError != MemError::None; }

inline const char* ErrorString(MemError e) {
    switch (e) {
        case MemError::None:           return "None";
        case MemError::InvalidAddress: return "InvalidAddress";
        case MemError::ReadFailed:     return "ReadFailed";
        case MemError::WriteFailed:    return "WriteFailed";
        case MemError::SyscallFailed:  return "SyscallFailed";
        case MemError::IOWriteFailed:  return "IOWriteFailed";
        case MemError::OpenFailed:     return "OpenFailed";
        case MemError::NotInitialized: return "NotInitialized";
    }
    return "Unknown";
}

namespace detail {
    inline pid_t g_pid = 0;
    inline KittyMemoryMgr g_mgr;   // RW: pvm syscall
    inline KittyMemIO     g_io;    // ForceRW: /proc/<pid>/mem (FOLL_FORCE)
} // namespace detail

inline bool Init(const std::string& processName = "") {
    ClearError();
    detail::g_pid = processName.empty() ? getpid() : KittyMemoryEx::getProcessID(processName);
    if (detail::g_pid < 1) {
        SetError(MemError::NotInitialized);
        KT_LOGE("[KittyEx] Init error: failed to get pid: %d", detail::g_pid);
        return false;
    }
    if (!detail::g_mgr.initialize(detail::g_pid, EK_MEM_OP_SYSCALL, false)) {
        SetError(MemError::NotInitialized);
        KT_LOGE("[KittyEx] Init error: failed to init KittyMemoryMgr");
        return false;
    }
    int orig_dumpable = prctl(PR_GET_DUMPABLE);
    if (orig_dumpable != 1) prctl(PR_SET_DUMPABLE, 1);
    bool io_ok = detail::g_io.init(detail::g_pid);
    if (orig_dumpable != 1) prctl(PR_SET_DUMPABLE, orig_dumpable >= 0 ? orig_dumpable : 0);
    if (!io_ok) {
        KT_LOGE("[KittyEx] Init error: KittyMemIO init failed (ForceRead/ForceWrite unavailable)");
        return false;
    }
    KT_LOGI("[KittyEx] Init success, pid: %d", detail::g_pid);
    return true;
}

inline pid_t GetPid() { return detail::g_pid; }

inline void ElfScan(const std::string& elfName, ElfScanner& scanner) {
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        scanner = detail::g_mgr.elfScanner.findElf(elfName);
    } while (!scanner.isValid());
}

inline std::vector<ElfScanner> GetAllELFs() {
    return detail::g_mgr.elfScanner.getAllELFs();
}

using ProcMap = KittyMemoryEx::ProcMap;

inline std::vector<ProcMap> GetAllMaps() {
    return KittyMemoryEx::getAllMaps(detail::g_pid);
}

// FastRW: in-process 直接 memcpy / LDR；无 syscall，LDR 会 demand-fault 冷 page
inline bool FastRead(uint64_t address, void* buffer, size_t len) {
    std::memcpy(buffer, reinterpret_cast<const void*>(address), len);
    return true;
}

inline bool FastWrite(uint64_t address, const void* data, size_t len) {
    std::memcpy(reinterpret_cast<void*>(address), data, len);
    return true;
}

inline uintptr_t FindDataFirst(uintptr_t start, uintptr_t end, const void* data, size_t size) {
    return detail::g_mgr.memScanner.findDataFirst(start, end, data, size);
}

inline uintptr_t FindIdaPatternFirst(uintptr_t start, uintptr_t end, const std::string& pattern) {
    return detail::g_mgr.memScanner.findIdaPatternFirst(start, end, pattern);
}

// FindXxxFast: in-process 扫描；直接 LDR 步进，冷 page 自动 demand-fault
inline uintptr_t FindDataFirstFast(uintptr_t start, uintptr_t end, const void* data, size_t size) {
    if (end <= start || !data || size == 0 || (end - start) < size) return 0;
    const uint8_t* hay    = reinterpret_cast<const uint8_t*>(start);
    const uint8_t* needle = reinterpret_cast<const uint8_t*>(data);
    const size_t span = (end - start) - size + 1;
    for (size_t i = 0; i < span; ++i) {
        if (std::memcmp(hay + i, needle, size) == 0)
            return start + i;
    }
    return 0;
}

inline uintptr_t FindIdaPatternFirstFast(uintptr_t start, uintptr_t end, const std::string& pattern) {
    std::vector<uint8_t> bytes, mask;  // mask: 0xFF=literal, 0x00=wildcard
    bytes.reserve(pattern.size() / 3 + 1);
    mask.reserve(pattern.size() / 3 + 1);
    const size_t plen = pattern.size();
    for (size_t i = 0; i < plen; ++i) {
        char c = pattern[i];
        if (c == ' ') continue;
        if (c == '?') {
            bytes.push_back(0); mask.push_back(0);
            // 支持单 '?' 或 "??"
            if (i + 1 < plen && pattern[i + 1] == '?') ++i;
            continue;
        }
        if (i + 1 < plen
            && std::isxdigit(static_cast<unsigned char>(c))
            && std::isxdigit(static_cast<unsigned char>(pattern[i + 1])))
        {
            bytes.push_back(static_cast<uint8_t>(
                std::stoi(pattern.substr(i, 2), nullptr, 16)));
            mask.push_back(0xFF);
            ++i;
        }
    }
    const size_t n = bytes.size();
    if (n == 0 || end <= start || (end - start) < n) return 0;
    const uint8_t* hay = reinterpret_cast<const uint8_t*>(start);
    const size_t span = (end - start) - n + 1;
    for (size_t i = 0; i < span; ++i) {
        const uint8_t* p = hay + i;
        size_t k = 0;
        for (; k < n; ++k) if (mask[k] && p[k] != bytes[k]) break;
        if (k == n) return start + i;
    }
    return 0;
}

// RW: 默认读写；走 process_vm_readv/writev syscall，跨进程兼容
inline bool Read(uint64_t address, void* buffer, size_t len) {
    size_t ret = detail::g_mgr.readMem(address, buffer, len);
    if (ret != len) { SetError(MemError::ReadFailed); return false; }
    return true;
}

inline bool Write(uint64_t address, const void* data, size_t len) {
    size_t ret = detail::g_mgr.writeMem(address, const_cast<void*>(data), len);
    if (ret != len) { SetError(MemError::WriteFailed); return false; }
    return true;
}

// ForceRW: /proc/<pid>/mem 路径
inline bool ForceRead(uint64_t address, void* buffer, size_t len) {
    if (detail::g_io.processID() != detail::g_pid) {
        SetError(MemError::NotInitialized); return false;
    }
    size_t ret = detail::g_io.Read(static_cast<uintptr_t>(address), buffer, len);
    if (ret != len) { SetError(MemError::ReadFailed); return false; }
    return true;
}

inline bool ForceWrite(uint64_t address, const void* data, size_t len) {
    if (detail::g_io.processID() != detail::g_pid) {
        SetError(MemError::NotInitialized); return false;
    }
    size_t ret = detail::g_io.Write(static_cast<uintptr_t>(address),
                                    const_cast<void*>(data), len);
    if (ret != len) { SetError(MemError::WriteFailed); return false; }
    return true;
}

// 指针链读：base->[+o1]->[+o2]->...->[+on] 读 Ret，链中任一段无效返回零值
namespace detail {
using RawReadFn = bool(*)(uint64_t, void*, size_t);

template<RawReadFn ReadFn, typename Ret, typename T, typename... Offsets>
inline Ret ChainRead(T base, Offsets... offsets) {
    uint64_t address = (uint64_t)base;
    if constexpr (sizeof...(offsets) == 0) {
        Ret value{};
        ReadFn(address, &value, sizeof(Ret));
        return value;
    } else {
        std::array<uint64_t, sizeof...(offsets)> offset_array = {static_cast<uint64_t>(offsets)...};
        for (size_t i = 0; i < sizeof...(offsets) - 1; ++i) {
            uint64_t next = 0;
            if (!ReadFn(address + offset_array[i], &next, sizeof(next)) || !IsValid(next))
                return Ret{};
            address = next;
        }
        Ret value{};
        ReadFn(address + offset_array[sizeof...(offsets) - 1], &value, sizeof(Ret));
        return value;
    }
}
} // namespace detail

template<typename Ret, typename T, typename... Offsets>
inline Ret FastRead(T base, Offsets... offsets) {
    return detail::ChainRead<FastRead, Ret>(base, offsets...);
}

template<typename Ret, typename T, typename... Offsets>
inline Ret Read(T base, Offsets... offsets) {
    return detail::ChainRead<Read, Ret>(base, offsets...);
}

template<typename Ret, typename T, typename... Offsets>
inline Ret ForceRead(T base, Offsets... offsets) {
    return detail::ChainRead<ForceRead, Ret>(base, offsets...);
}

// 整型/POD 写：FastWrite/Write/ForceWrite(addr, value)
template<typename T>
inline typename std::enable_if<
    !std::is_convertible<T, std::string>::value &&
    !std::is_convertible<T, std::vector<uint8_t>>::value,
    bool>::type
FastWrite(uintptr_t address, T value) {
    return FastWrite(address, &value, sizeof(T));
}

template<typename T>
inline typename std::enable_if<
    !std::is_convertible<T, std::string>::value &&
    !std::is_convertible<T, std::vector<uint8_t>>::value,
    bool>::type
Write(uintptr_t address, T value) {
    return Write(address, &value, sizeof(T));
}

template<typename T>
inline typename std::enable_if<
    !std::is_convertible<T, std::string>::value &&
    !std::is_convertible<T, std::vector<uint8_t>>::value,
    bool>::type
ForceWrite(uintptr_t address, T value) {
    return ForceWrite(address, &value, sizeof(T));
}

// hex string / byte array 写入
inline bool ConvStringToByteArray(const std::string& hexString, std::vector<uint8_t>& byteArray) {
    if (hexString.size() < 2 || hexString.size() % 2 != 0) return false;
    for (size_t i = 0; i < hexString.size(); i += 2) {
        int byte = std::stoi(hexString.substr(i, 2), nullptr, 16);
        byteArray.push_back(static_cast<uint8_t>(byte));
    }
    return true;
}

inline bool Write(uintptr_t address, const std::string& hexString) {
    std::vector<uint8_t> byteArray;
    ConvStringToByteArray(hexString, byteArray);
    return Write(address, byteArray.data(), byteArray.size());
}

inline bool Write(uintptr_t address, const std::vector<uint8_t>& byteArray) {
    return Write(address, (void*)byteArray.data(), byteArray.size());
}

}

#pragma once

#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#define __int8 char
#define __int16 short
#define __int32 int
#define __int64 long long

typedef char _BYTE;
typedef short _WORD;
typedef int _DWORD;
typedef long long _QWORD;

#define ASM_MOV(__reg) ({       \
    uintptr_t __val;            \
    asm __volatile__ (          \
        "mov %0, " #__reg ";"   \
        : "=r" (__val)          \
        :                       \
        :                       \
    );                          \
    __val;                      \
})

#define MAKE_CRASH()     \
    __asm__ volatile (   \
        "mov x0, xzr;"   \
        "mov x29, x0;"   \
        "mov sp, x0;"    \
        "br x0;"         \
        : : :            \
    );

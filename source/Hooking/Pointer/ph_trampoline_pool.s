// In-binary trampoline pool — n × 32 B slots, brk-prefilled.

    .section .ph_trampoline_pool, "ax", %progbits
    .balign 4096

    .global __ph_trampoline_pool_start
    .hidden __ph_trampoline_pool_start
__ph_trampoline_pool_start:

    .rept 2048
        .word 0xD4200000    // brk #0
        .word 0xD4200000
        .word 0xD4200000
        .word 0xD4200000
        .word 0xD4200000
        .word 0xD4200000
        .word 0xD4200000
        .word 0xD4200000
    .endr

    .global __ph_trampoline_pool_end
    .hidden __ph_trampoline_pool_end
__ph_trampoline_pool_end:

    .size __ph_trampoline_pool_start, . - __ph_trampoline_pool_start

#if defined(__linux__) && !defined(__APPLE__)
    .section .note.GNU-stack, "", %progbits
#endif

// arm64 glue — register-save bridge from a per-hook trampoline

    .global _ph_glue_entry
    .hidden _ph_glue_entry
    .extern _ph_dispatcher
    .text
    .align 4

_ph_glue_entry:
    bti     jc

    sub     sp, sp, #0x310

    str     x17, [sp, #0x308]
    mrs     x16, nzcv
    add     x17, sp, #0x200
    stp     x0,  x1,  [x17, #0x00]
    stp     x2,  x3,  [x17, #0x10]
    stp     x4,  x5,  [x17, #0x20]
    stp     x6,  x7,  [x17, #0x30]
    stp     x8,  x9,  [x17, #0x40]
    stp     x10, x11, [x17, #0x50]
    stp     x12, x13, [x17, #0x60]
    stp     x14, x15, [x17, #0x70]
    stp     xzr, xzr, [x17, #0x80]
    stp     x18, x19, [x17, #0x90]
    stp     x20, x21, [x17, #0xA0]
    stp     x22, x23, [x17, #0xB0]
    stp     x24, x25, [x17, #0xC0]
    stp     x26, x27, [x17, #0xD0]
    stp     x28, x29, [x17, #0xE0]

    add     x18, sp, #0x310             // orig SP (x18 is already saved)
    stp     x30, x18, [x17, #0xF0]      // lr @+0xF0, orig sp @+0xF8
    str     x16, [x17, #0x100]          // nzcv @+0x100

    stp     q0,  q1,  [sp, #0x000]
    stp     q2,  q3,  [sp, #0x020]
    stp     q4,  q5,  [sp, #0x040]
    stp     q6,  q7,  [sp, #0x060]
    stp     q8,  q9,  [sp, #0x080]
    stp     q10, q11, [sp, #0x0A0]
    stp     q12, q13, [sp, #0x0C0]
    stp     q14, q15, [sp, #0x0E0]
    stp     q16, q17, [sp, #0x100]
    stp     q18, q19, [sp, #0x120]
    stp     q20, q21, [sp, #0x140]
    stp     q22, q23, [sp, #0x160]
    stp     q24, q25, [sp, #0x180]
    stp     q26, q27, [sp, #0x1A0]
    stp     q28, q29, [sp, #0x1C0]
    stp     q30, q31, [sp, #0x1E0]

    // Call _ph_dispatcher(ctx = sp, hook = x1)
    mov     x0, sp
    ldr     x1, [sp, #0x308]
    str     xzr, [sp, #0x308]
    bl      _ph_dispatcher

    // x0 = continuation: 0 → skip original (RET), non-zero → BR to it.
    mov     x16, x0

    // ---- FP / SIMD restore ----
    ldp     q0,  q1,  [sp, #0x000]
    ldp     q2,  q3,  [sp, #0x020]
    ldp     q4,  q5,  [sp, #0x040]
    ldp     q6,  q7,  [sp, #0x060]
    ldp     q8,  q9,  [sp, #0x080]
    ldp     q10, q11, [sp, #0x0A0]
    ldp     q12, q13, [sp, #0x0C0]
    ldp     q14, q15, [sp, #0x0E0]
    ldp     q16, q17, [sp, #0x100]
    ldp     q18, q19, [sp, #0x120]
    ldp     q20, q21, [sp, #0x140]
    ldp     q22, q23, [sp, #0x160]
    ldp     q24, q25, [sp, #0x180]
    ldp     q26, q27, [sp, #0x1A0]
    ldp     q28, q29, [sp, #0x1C0]
    ldp     q30, q31, [sp, #0x1E0]

    add     x17, sp, #0x200
    ldr     x18, [x17, #0x100]
    msr     nzcv, x18
    ldp     x2,  x3,  [x17, #0x10]
    ldp     x4,  x5,  [x17, #0x20]
    ldp     x6,  x7,  [x17, #0x30]
    ldp     x8,  x9,  [x17, #0x40]
    ldp     x10, x11, [x17, #0x50]
    ldp     x12, x13, [x17, #0x60]
    ldp     x14, x15, [x17, #0x70]
    ldp     x18, x19, [x17, #0x90]
    ldp     x20, x21, [x17, #0xA0]
    ldp     x22, x23, [x17, #0xB0]
    ldp     x24, x25, [x17, #0xC0]
    ldp     x26, x27, [x17, #0xD0]
    ldp     x28, x29, [x17, #0xE0]
    ldr     x30, [x17, #0xF0]
    ldp     x0,  x1,  [x17, #0x00]

    add     sp, sp, #0x310

    cbz     x16, 1f
    br      x16
1:  ret

    .size _ph_glue_entry, . - _ph_glue_entry



//  Backend for IPointerHook::CallOrigWithContext<Ret>(ctx, spill_bytes).

    .global _ph_call_orig_with_ctx_i
    .hidden _ph_call_orig_with_ctx_i
    .global _ph_call_orig_with_ctx_d
    .hidden _ph_call_orig_with_ctx_d
    .align 4

_ph_call_orig_with_ctx_i:
_ph_call_orig_with_ctx_d:
    bti     c

    stp     x29, x30, [sp, #-32]!
    stp     x19, x20, [sp, #16]
    mov     x29, sp

    mov     x19, x0                     // ctx
    mov     x20, x1                     // func

    // Mirror overflow spill if any. Clamp to 128, round up to 16.
    cbz     x2, 2f
    mov     x9, #128
    cmp     x2, x9
    csel    x2, x9, x2, hi
    add     x2, x2, #15
    bic     x2, x2, #15
    sub     sp, sp, x2
    ldr     x9, [x19, #0x2F8]           // src = ctx->sp
    mov     x10, sp                     // dst = our new sp
1:
    ldp     x11, x12, [x9], #16
    stp     x11, x12, [x10], #16
    subs    x2, x2, #16
    b.ne    1b

2:
    ldr     x9, [x19, #0x300]
    msr     nzcv, x9

    ldp     q0, q1, [x19, #0x000]
    ldp     q2, q3, [x19, #0x020]
    ldp     q4, q5, [x19, #0x040]
    ldp     q6, q7, [x19, #0x060]

    // LDP x-reg imm only encodes [-512, 504] — rebase first.
    add     x9, x19, #0x200
    ldp     x0, x1, [x9, #0x00]
    ldp     x2, x3, [x9, #0x10]
    ldp     x4, x5, [x9, #0x20]
    ldp     x6, x7, [x9, #0x30]
    ldr     x8,     [x9, #0x40]

    blr     x20

    mov     sp, x29
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret

.Lph_call_orig_with_ctx_end:
    .size _ph_call_orig_with_ctx_i, .Lph_call_orig_with_ctx_end - _ph_call_orig_with_ctx_i
    .size _ph_call_orig_with_ctx_d, .Lph_call_orig_with_ctx_end - _ph_call_orig_with_ctx_d


#if defined(__linux__) && !defined(__APPLE__)
    .section .note.GNU-stack, "", %progbits
    .section .note.gnu.property, "a"
    .align 3
    .long 4
    .long 16
    .long 5
    .asciz "GNU"
    .align 3
    .long 0xc0000000
    .long 4
    .long 1
    .long 0
#endif

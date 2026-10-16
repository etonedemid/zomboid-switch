// GL call tracer stubs (debug, enabled by sdmc:/pz/gltrace): stub i puts i in x16 and jumps to the common path, which
// asks lx_gltrace(i) for the real function with every argument register preserved, then tail-calls it.
    .text
    .global lx_gltrace_stubs
    .p2align 3
lx_gltrace_stubs:
    .set i, 0
    .rept 4096
    movz x16, #i
    b 1f
    .set i, i + 1
    .endr

1:  stp x29, x30, [sp, #-224]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    mov x0, x16
    bl lx_gltrace
    mov x16, x0
    ldp q6, q7, [sp, #192]
    ldp q4, q5, [sp, #160]
    ldp q2, q3, [sp, #128]
    ldp q0, q1, [sp, #96]
    ldr x8, [sp, #80]
    ldp x6, x7, [sp, #64]
    ldp x4, x5, [sp, #48]
    ldp x2, x3, [sp, #32]
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #224
    br x16

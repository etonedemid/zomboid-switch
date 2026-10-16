// Fixed pool of JNI entry trampolines for emulated (x86-64) native methods - no runtime code generation.
// Trampoline i: x9 = &lx_x86_ctx[i], jump to lx_x86_entry. lx_x86_entry saves the AAPCS64 argument registers and
// calls lx_x86_call(ctx, gp[8], fp[8], stack_args, &fpret); the integer result comes back in x0 and a float/double
// result in fpret, loaded into d0 (callers only look at the register their return type uses).
#define NTRAMP 4096
#define CTX_SIZE 16

.text
.balign 16
.global lx_x86_tramps
lx_x86_tramps:
.set i, 0
.rept NTRAMP
    adrp x9, lx_x86_ctx
    add x9, x9, :lo12:lx_x86_ctx
    add x9, x9, #((i * CTX_SIZE) & 0xfff)
    add x9, x9, #((i * CTX_SIZE) >> 12), lsl #12
    b lx_x86_entry
    .balign 32
    .set i, i + 1
.endr

.global lx_x86_entry
.type lx_x86_entry, %function
lx_x86_entry:
    stp x29, x30, [sp, #-160]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp d0, d1, [sp, #80]
    stp d2, d3, [sp, #96]
    stp d4, d5, [sp, #112]
    stp d6, d7, [sp, #128]
    mov x0, x9              // ctx
    add x1, sp, #16         // gp regs
    add x2, sp, #80         // fp regs
    add x3, sp, #160        // caller's stack arguments
    add x4, sp, #144        // fp return slot
    bl lx_x86_call
    ldr d0, [sp, #144]
    ldp x29, x30, [sp], #160
    ret

.bss
.balign 16
.global lx_x86_ctx
lx_x86_ctx:
    .skip NTRAMP * CTX_SIZE

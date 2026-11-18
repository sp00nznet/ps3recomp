// Apple AArch64, AAPCS64 2026Q3 6.1: x19-x29, SP, low v8-v15.
// Opt-in: macOS still defaults to ucontext, which has no raw register record.
// x18 is reserved. FPCR is global, FPSR caller-saved. Only the context record
// is written, preserving both red zones. SME/MTE stacks are not qualified.
#include "host_fiber_ctx.h"
#if !defined(__APPLE__) || !defined(__aarch64__)
#error "host_fiber_aarch64.S implements the Apple AArch64 ABI only"
#endif
#if defined(__arm64e__)
#error "arm64e pointer-authenticated returns are not qualified"
#endif
.text
.p2align 4
.globl _ps3_host_ctx_switch
.private_extern _ps3_host_ctx_switch
_ps3_host_ctx_switch:
.cfi_startproc
    stp x19, x20, [x0, #HOST_CTX_X19]
    stp x21, x22, [x0, #HOST_CTX_X21]
    stp x23, x24, [x0, #HOST_CTX_X23]
    stp x25, x26, [x0, #HOST_CTX_X25]
    stp x27, x28, [x0, #HOST_CTX_X27]
    stp x29, x30, [x0, #HOST_CTX_FP]
    mov x9, sp
    str x9, [x0, #HOST_CTX_SP]
    stp d8, d9, [x0, #HOST_CTX_D8]
    stp d10, d11, [x0, #HOST_CTX_D10]
    stp d12, d13, [x0, #HOST_CTX_D12]
    stp d14, d15, [x0, #HOST_CTX_D14]
    ldp x19, x20, [x1, #HOST_CTX_X19]
    ldp x21, x22, [x1, #HOST_CTX_X21]
    ldp x23, x24, [x1, #HOST_CTX_X23]
    ldp x25, x26, [x1, #HOST_CTX_X25]
    ldp x27, x28, [x1, #HOST_CTX_X27]
    ldp x29, x30, [x1, #HOST_CTX_FP]
    ldp d8, d9, [x1, #HOST_CTX_D8]
    ldp d10, d11, [x1, #HOST_CTX_D10]
    ldp d12, d13, [x1, #HOST_CTX_D12]
    ldp d14, d15, [x1, #HOST_CTX_D14]
    ldr x9, [x1, #HOST_CTX_SP]
    mov sp, x9
    ret
.cfi_endproc

// New contexts have x19=f and x29 pointing at a zero frame record.
.p2align 2
.globl _ps3_host_fiber_trampoline
.private_extern _ps3_host_fiber_trampoline
_ps3_host_fiber_trampoline:
.cfi_startproc
.cfi_undefined x30
    mov x0, x19
    bl _ps3_host_fiber_start
    brk #0xf1be
.cfi_endproc

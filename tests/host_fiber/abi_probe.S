// Two-sided ABI probe: no C prologue runs on either side, so a switch that
// drops a register fails here instead of hiding in a caller's save area.
#include "host_fiber_ctx.h"
.text
.macro address reg, symbol
    adrp \reg, \symbol@PAGE
    add \reg, \reg, \symbol@PAGEOFF
.endm
.macro save base
    stp x19, x20, [\base, #HOST_CTX_X19]
    stp x21, x22, [\base, #HOST_CTX_X21]
    stp x23, x24, [\base, #HOST_CTX_X23]
    stp x25, x26, [\base, #HOST_CTX_X25]
    stp x27, x28, [\base, #HOST_CTX_X27]
    stp x29, x30, [\base, #HOST_CTX_FP]
    mov x10, sp
    str x10, [\base, #HOST_CTX_SP]
    stp d8, d9, [\base, #HOST_CTX_D8]
    stp d10, d11, [\base, #HOST_CTX_D10]
    stp d12, d13, [\base, #HOST_CTX_D12]
    stp d14, d15, [\base, #HOST_CTX_D14]
.endm
.macro load base
    ldp x19, x20, [\base, #HOST_CTX_X19]
    ldp x21, x22, [\base, #HOST_CTX_X21]
    ldp x23, x24, [\base, #HOST_CTX_X23]
    ldp x25, x26, [\base, #HOST_CTX_X25]
    ldp x27, x28, [\base, #HOST_CTX_X27]
    ldr x29, [\base, #HOST_CTX_FP]
    ldp d8, d9, [\base, #HOST_CTX_D8]
    ldp d10, d11, [\base, #HOST_CTX_D10]
    ldp d12, d13, [\base, #HOST_CTX_D12]
    ldp d14, d15, [\base, #HOST_CTX_D14]
.endm
.globl _hf_abi_run
_hf_abi_run:
    sub sp, sp, #HOST_CTX_SIZE
    mov x9, sp
    save x9
    // A broken switch can return on the peer stack. Keep the way home so the
    // epilogue still reaches the C checks, which then fail by name.
    address x10, _hf_abi_home
    str x9, [x10]
    address x9, _hf_abi_a
    mov x10, sp
    str x10, [x9, #HOST_CTX_SP]
    load x9
    address x0, _hf_abi_runner
    address x1, _hf_abi_peer
    bl _ps3_host_ctx_switch
    address x9, _hf_abi_got_a
    save x9
    address x9, _hf_abi_c
    mov x10, sp
    str x10, [x9, #HOST_CTX_SP]
    load x9
    address x0, _hf_abi_runner2
    address x1, _hf_abi_peer
    bl _ps3_host_ctx_switch
    address x9, _hf_abi_got_c
    save x9
    address x9, _hf_abi_home
    ldr x9, [x9]
    mov sp, x9
    load x9
    ldr x30, [sp, #HOST_CTX_FP+8]
    add sp, sp, #HOST_CTX_SIZE
    ret

// No C prologue on either side can save a missing register restoration.
.globl _hf_abi_peer_entry
_hf_abi_peer_entry:
    address x9, _hf_abi_b
    load x9
    address x0, _hf_abi_peer
    address x1, _hf_abi_runner
    bl _ps3_host_ctx_switch
    address x9, _hf_abi_got_b
    save x9
    address x0, _hf_abi_peer2
    address x1, _hf_abi_runner2
    bl _ps3_host_ctx_switch
    brk #1

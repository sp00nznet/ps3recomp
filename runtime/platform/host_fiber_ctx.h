/*
 * ps3recomp - AArch64 host context record
 *
 * The HOST_CTX_* offsets are shared with host_fiber_aarch64.S and the struct
 * below is laid out to match them; the static assertions catch a field reorder
 * before it becomes an assembly/C disagreement.
 */
#ifndef PS3_HOST_FIBER_CTX_H
#define PS3_HOST_FIBER_CTX_H
#define HOST_CTX_X19  0x00
#define HOST_CTX_X21  0x10
#define HOST_CTX_X23  0x20
#define HOST_CTX_X25  0x30
#define HOST_CTX_X27  0x40
#define HOST_CTX_FP   0x50
#define HOST_CTX_SP   0x60
#define HOST_CTX_D8   0x70
#define HOST_CTX_D10  0x80
#define HOST_CTX_D12  0x90
#define HOST_CTX_D14  0xA0
#define HOST_CTX_SIZE 0xB0
#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
typedef struct ps3_host_ctx {
    _Alignas(16) uint64_t x19_x28[10];
    uint64_t fp, lr, sp, pad;
    uint64_t d8_d15[8];
} ps3_host_ctx;
_Static_assert(offsetof(ps3_host_ctx, fp) == HOST_CTX_FP, "x29 slot");
_Static_assert(offsetof(ps3_host_ctx, sp) == HOST_CTX_SP, "sp slot");
_Static_assert(offsetof(ps3_host_ctx, d8_d15) == HOST_CTX_D8, "d8 slot");
_Static_assert(sizeof(ps3_host_ctx) == HOST_CTX_SIZE, "record size");
#endif
#endif

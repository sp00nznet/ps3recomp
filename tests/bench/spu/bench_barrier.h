/* Force-included into the lifted benchmark kernels (run_spu_bench.sh).
 *
 * A kernel repeats one operation 32 times on loop-invariant operands, which
 * an optimizing compiler folds (32 x `a $3,$3,$4` became one $3 + 4*$4) or
 * hoists out of the loop, timing the compiler instead of the operation.
 * apply_barriers.py wraps every result: `ctx->gpr[N] = bench_opaque(expr);`.
 *
 * bench_opaque passes the value through an empty asm that claims to change
 * it, in a vector register: nothing folds across it, and it costs no
 * instruction. It acts on the value, not on ctx->gpr[N] in memory, and it
 * is not volatile -- clang treats a volatile asm as touching memory, and both
 * mistakes turned every op into load + op + store through ctx->gpr, which
 * game code does not pay. A chained kernel's input is loop-carried, so the
 * plain asm can be neither CSE'd nor hoisted. Kernels whose op reads no
 * register (il, fsmbi...) use bench_opaque_v, which is volatile, or the
 * constant would be hoisted out of the loop. Load and store kernels also get
 * BENCH_CLOBBER, so invariant loads are not hoisted and repeated stores are
 * not sunk out of the loop. */
#ifndef BENCH_BARRIER_H
#define BENCH_BARRIER_H
#include <string.h>
#include "ps3emu/ps3types.h"
#if defined(__aarch64__)
#include <arm_neon.h>
typedef uint8x16_t bench_vec;
#define BENCH_VREG "+w"
#elif defined(__x86_64__)
#include <emmintrin.h>
typedef __m128i bench_vec;
#define BENCH_VREG "+x"
#else
#error "bench_barrier.h: add a vector-register constraint for this host"
#endif
static inline __attribute__((always_inline)) u128 bench_opaque(u128 v)
{
    bench_vec t; memcpy(&t, &v, 16);
    __asm__("" : BENCH_VREG(t));
    memcpy(&v, &t, 16); return v;
}
static inline __attribute__((always_inline)) u128 bench_opaque_v(u128 v)
{
    bench_vec t; memcpy(&t, &v, 16);
    __asm__ volatile("" : BENCH_VREG(t));
    memcpy(&v, &t, 16); return v;
}
#define BENCH_CLOBBER() __asm__ volatile("" ::: "memory")
#endif

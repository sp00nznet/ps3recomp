/*
 * ps3recomp - vertex-program runs in one call
 *
 * rsx_process_vp_run takes a whole incrementing run of vertex-program
 * microcode (0x0B80-0x0BFC) or constants (the 0x1EFC load slot, then
 * 0x1F00-0x1F7C) from guest memory in one call. Whatever it takes must leave
 * rsx_state exactly as the same words do through rsx_process_method one by
 * one, including microcode that rewrites the bytes already there, and runs
 * that leave either window must be refused untouched.
 *
 *   gcc -std=gnu17 -Wall -Wextra -I include -I libs/video \
 *       -o /tmp/test_rsx_vp_run \
 *       libs/video/tests/test_rsx_vp_run.c libs/video/rsx_commands.c
 */
#include "rsx_commands.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* rsx_commands.c reaches the guest VM and the boot milestone log; stubs are enough to link. */
uint32_t ppu_hle_inject_base;
void vm_write32(uint32_t addr, uint32_t value) { (void)addr; (void)value; }
void ps3_ms(const char* key) { (void)key; }
uint32_t gcm_io_offset_to_ea(uint32_t io) { (void)io; return 0; }
unsigned long long ps3_ms_now(void) { return 0; }

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

static rsx_state a, b;   /* a: through the run call, b: word by word */

/* The words as the walker sees them in guest memory: big-endian. */
static void run(u32 method, const u32* words, u32 count, int* took)
{
    u8 src[64 * 4];
    for (u32 i = 0; i < count; i++) {
        src[i * 4 + 0] = (u8)(words[i] >> 24); src[i * 4 + 1] = (u8)(words[i] >> 16);
        src[i * 4 + 2] = (u8)(words[i] >> 8);  src[i * 4 + 3] = (u8)words[i];
    }
    *took = rsx_process_vp_run(&a, method, src, count);
    for (u32 i = 0; i < count; i++) {
        if (!*took) rsx_process_method(&a, method + i * 4, words[i]);
        rsx_process_method(&b, method + i * 4, words[i]);
    }
}

int main(void)
{
    rsx_state_init(&a);
    rsx_state_init(&b);
    int fails = 0;
    long taken_ucode = 0, taken_const = 0, refused = 0, wrongly_taken = 0;
    u32 last_ucode[32] = {0};

    for (int it = 0; it < 200000; it++) {
        u32 words[64];
        int took = 0;
        const u32 kind = rnd() % 8;
        if (kind == 0) {
            const u32 load = rnd() % 520;   /* a few past the end of the program store */
            rsx_process_method(&a, NV4097_SET_TRANSFORM_PROGRAM_LOAD, load);
            rsx_process_method(&b, NV4097_SET_TRANSFORM_PROGRAM_LOAD, load);
        } else if (kind <= 3) {             /* microcode, sometimes past the window */
            const u32 first = rnd() % 32, count = 1 + rnd() % 34;
            const int same = rnd() % 3 == 0;   /* rewrite what is there */
            for (u32 i = 0; i < count; i++) words[i] = same ? last_ucode[(first + i) % 32] : rnd();
            for (u32 i = 0; i < count && first + i < 32; i++) last_ucode[first + i] = words[i];
            run(NV4097_SET_TRANSFORM_PROGRAM + first * 4, words, count, &took);
            const int inside = first + count <= 32;
            if (took) { taken_ucode++; wrongly_taken += !inside; } else { refused++; wrongly_taken += inside; }
        } else if (kind <= 6) {             /* constants, from the load slot or inside the window */
            const int from_load = rnd() % 2;
            const u32 first = from_load ? 0 : rnd() % 32;
            const u32 count = 1 + rnd() % 34;
            for (u32 i = 0; i < count; i++) words[i] = rnd();
            if (from_load) words[0] = rnd() % 540;   /* the load slot: a constant index */
            const u32 method = from_load ? NV4097_SET_TRANSFORM_CONSTANT_LOAD
                                         : NV4097_SET_TRANSFORM_CONSTANT + first * 4;
            run(method, words, count, &took);
            const int inside = method + count * 4 <=
                               NV4097_SET_TRANSFORM_CONSTANT + NV4097_SET_TRANSFORM_CONSTANT_DWORDS * 4;
            if (took) { taken_const++; wrongly_taken += !inside; } else { refused++; wrongly_taken += inside; }
        } else {                            /* the backend consumed the dirty state */
            a.vp_dirty = b.vp_dirty = 0;
            a.vertex_constants_dirty = b.vertex_constants_dirty = 0;
        }
        if (memcmp(&a, &b, sizeof a) != 0) {
            printf("[FAIL] state differs after step %d (kind %u, took %d)\n", it, kind, took);
            fails++;
            break;
        }
    }
    printf("[%s] microcode runs taken %ld, constant runs taken %ld, refused %ld\n",
           wrongly_taken ? "FAIL" : "PASS", taken_ucode, taken_const, refused);
    fails += wrongly_taken != 0;
    if (!taken_ucode || !taken_const) { puts("[FAIL] a window was never taken"); fails++; }
    puts(fails ? "test_rsx_vp_run: FAIL" : "test_rsx_vp_run: PASS");
    return fails ? 1 : 0;
}

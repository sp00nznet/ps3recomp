/*
 * rsx_vp_analyze_constants against the decompiler it describes.
 *
 * The draw engine uploads only the vp_c[] rows this analysis reports, so a row
 * it misses is a constant the shader reads as stale data. The check here is
 * not against a second decoding of the fields but against the HLSL the
 * decompiler actually emits: random programs are decompiled, every vp_c[...]
 * in the text is collected, and each fixed index must fall inside the
 * reported range, and a relative read in the shader must be reported as
 * `relative` (the converse is allowed: it only costs a full upload).
 *
 * Build (from the repo root):
 *   clang -std=gnu17 -Wall -Wextra -I include -I libs/video \
 *     -o /tmp/test_vp_const_usage libs/video/tests/test_vp_const_usage.c \
 *     libs/video/rsx_vp_decompiler.c
 */
#include "rsx_vp_decompiler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails <= 20) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

static void put_le(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* One instruction with chosen source types. Opcodes are random over the
 * whole field; flow-control and unknown ops are left in on purpose, since
 * the analysis has to cover whatever the decompiler emits for them too. */
static void make_instr(uint8_t* p, int last, int relative_bias)
{
    uint32_t d0 = rnd(), d1 = rnd(), d2 = rnd(), d3 = rnd();
    /* Bias the source types toward constants (type 3) so most programs
     * exercise the analysis. */
    if (rnd() & 1) d1 = (d1 & ~0xFFu) | (((d1 & 0xFFu) & ~1u) | 1u);    /* src0 hi bits */
    if (rnd() & 1) d2 = (d2 & ~(3u << 6)) | (3u << 6);                  /* src1 type = 3 */
    if (rnd() & 1) d3 = (d3 & ~(3u << 21)) | (3u << 21);                /* src2 type low bits */
    if (rnd() & 1) d2 = (d2 & ~(3u << 23)) | (3u << 23);                /* src0 type = 3 */
    if (!relative_bias) d3 &= ~2u;          /* index_const off */
    d3 &= ~1u;
    if (last) d3 |= 1u;
    put_le(p + 0, d0); put_le(p + 4, d1); put_le(p + 8, d2); put_le(p + 12, d3);
}

static char hlsl[1 << 20];

int main(void)
{
    enum { PROGRAMS = 20000, MAXI = 24 };
    static uint8_t uc[MAXI * 16];
    int decompiled = 0, ranged = 0, relative = 0, over_relative = 0;
    for (int n = 0; n < PROGRAMS; n++) {
        const int instrs = 1 + (int)(rnd() % MAXI);
        const int rel = (rnd() % 4) == 0;
        for (int i = 0; i < instrs; i++) make_instr(uc + i * 16, i == instrs - 1, rel);

        rsx_vp_const_usage u;
        const int ai = rsx_vp_analyze_constants(uc, (uint32_t)instrs * 16u, &u);
        CHECK(ai == instrs && u.valid, "program %d: analysis returned %d (valid %d) for %d instrs",
              n, ai, u.valid, instrs);

        const int r = rsx_vp_decompile(uc, (uint32_t)instrs * 16u, hlsl, sizeof hlsl);
        if (r < 0) continue;
        decompiled++;

        int saw_relative = 0;
        for (const char* q = hlsl; (q = strstr(q, "vp_c[")) != NULL; q += 5) {
            const char* idx = q + 5;
            /* The cbuffer declaration, not a read. */
            if (q - hlsl >= 7 && !strncmp(q - 7, "float4 ", 7)) continue;
            if (*idx == '(') { saw_relative = 1; continue; }
            const unsigned c = (unsigned)strtoul(idx, NULL, 10);
            CHECK(!u.relative ? (u.lo <= u.hi && c >= u.lo && c <= u.hi) : 1,
                  "program %d: shader reads vp_c[%u] outside [%u, %u]", n, c, u.lo, u.hi);
        }
        /* One-way: a relative read the analysis misses would leave rows
         * unwritten. Reporting relative for a source the opcode never reads
         * only costs a full copy, so it is allowed and counted. */
        CHECK(!saw_relative || u.relative,
              "program %d: shader reads relatively, analysis did not say so", n);
        if (u.relative && !saw_relative) over_relative++;
        if (u.relative) relative++; else if (u.lo <= u.hi) ranged++;
    }

    /* No end flag: the analysis must refuse to give a range. */
    for (int i = 0; i < 4; i++) make_instr(uc + i * 16, 0, 0);
    rsx_vp_const_usage u;
    CHECK(rsx_vp_analyze_constants(uc, 4 * 16u, &u) < 0 && !u.valid,
          "unterminated program accepted");
    /* NULL input. */
    CHECK(rsx_vp_analyze_constants(NULL, 16, &u) < 0 && !u.valid, "NULL ucode accepted");

    printf("test_vp_const_usage: %d programs decompiled (%d ranged, %d relative, %d of those"
           " conservatively), %d failure(s)\n", decompiled, ranged, relative, over_relative, fails);
    if (decompiled < PROGRAMS / 2 || ranged < 1000 || relative < 100) {
        printf("FAIL: too few programs exercised the check\n");
        return 1;
    }
    return fails ? 1 : 0;
}

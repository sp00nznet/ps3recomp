/*
 * ps3recomp -- cellAtrac against a real .at3 file
 *
 * Drives libs/codec/cellAtrac.c the way a title does (SetData, CreateDecoder,
 * SetLoopNum, Decode until finished) through a fake guest memory, and checks
 * what comes out:
 *   * exactly fact[0] samples per channel for one pass;
 *   * every sample finite and within [-2, 2], and not all silence;
 *   * with one loop, the second lap is sample-identical to the first -- the
 *     loop seam lands where the first pass started.
 * No .at3 ships with the repo; pass one (e.g. a RIFF cut out of a title's
 * data). A second argument writes the first pass as a 16-bit WAV to listen to.
 *
 * Build (from the repo root; no runtime lib needed):
 *   clang-cl /O2 -w /Iinclude libs/codec/tests/test_cellAtrac.c
 *            third_party/at3_standalone/*.cpp
 *   test_cellAtrac.exe track.at3 [out.wav]
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

uint8_t* vm_base;
#include "../cellAtrac.c"

/* The guest-memory accessors' hooks into the PPU/SPU runtime; inert here. */
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_linelock_lock(uint32_t a) { (void)a; }
void spu_linelock_release(uint32_t a) { (void)a; }

enum { MEM = 64u << 20, H = 0x100, WORK = 0x200, OUT = 0x10000, ARGS = 0x20,
       FILE_EA = 0x100000 };

static u32 rd32(u32 ea) { return vm_read32(ea); }

/* Decodes until finished; returns interleaved host floats (caller frees). */
static float* decode_all(u32 ch, s32 loops, u32* samples)
{
    assert(cellAtracSetLoopNum(H, loops) == CELL_OK);
    size_t cap = 1u << 20, n = 0;
    float* pcm = malloc(cap * sizeof(float));
    for (;;) {
        s32 rc = cellAtracDecode(H, OUT, ARGS, ARGS + 4, ARGS + 8);
        if (rc == (s32)CELL_ATRAC_ERROR_ALLDATA_WAS_DECODED) break;
        assert(rc == CELL_OK);
        const u32 got = rd32(ARGS);
        assert(got <= 2048);
        if (n + got * ch > cap) pcm = realloc(pcm, (cap *= 2) * sizeof(float));
        for (u32 i = 0; i < got * ch; i++) {
            u32 bits = rd32(OUT + i * 4);
            memcpy(&pcm[n + i], &bits, 4);
        }
        n += got * ch;
        if (rd32(ARGS + 4)) break;
    }
    *samples = (u32)(n / ch);
    return pcm;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s track.at3 [out.wav]\n", argv[0]); return 2; }
    vm_base = calloc(1, MEM);
    FILE* f = fopen(argv[1], "rb");
    assert(f);
    const u32 bytes = (u32)fread(vm_base + FILE_EA, 1, MEM - FILE_EA, f);
    fclose(f);

    AtracTrackInfo t;
    assert(atrac_parse_riff(vm_base + FILE_EA, bytes, &t) == 0);
    const u32 fact0 = (u32)(t.end - t.first);

    assert(cellAtracSetDataAndGetMemSize(H, FILE_EA, bytes, bytes, ARGS) == CELL_OK);
    assert(cellAtracCreateDecoder(H, WORK, 0, 0) == CELL_OK);
    assert(cellAtracGetChannel(H, ARGS) == CELL_OK);
    const u32 ch = rd32(ARGS);

    u32 n1;
    float* once = decode_all(ch, 0, &n1);
    assert(n1 == fact0);
    double sq = 0, peak = 0;
    for (u32 i = 0; i < n1 * ch; i++) {
        assert(isfinite(once[i]) && fabsf(once[i]) <= 2.0f);
        sq += (double)once[i] * once[i];
        if (fabs(once[i]) > peak) peak = fabs(once[i]);
    }
    const double rms = sqrt(sq / (n1 * ch));
    assert(rms > 1e-3);
    printf("%s: %u ch, %u samples (%.1f s), rms %.3f, peak %.3f\n",
           argv[1], ch, n1, n1 / 48000.0, rms, peak);

    if (t.loop_start >= 0) {
        /* Fresh start, so lap 1 is decoded continuously from frame 0 and
         * lap 2 through the seam's decoder restart. */
        assert(cellAtracDeleteDecoder(H) == CELL_OK);
        assert(cellAtracSetDataAndGetMemSize(H, FILE_EA, bytes, bytes, ARGS) == CELL_OK);
        assert(cellAtracCreateDecoder(H, WORK, 0, 0) == CELL_OK);
        u32 n2;
        float* twice = decode_all(ch, 1, &n2);
        const u32 lap = (u32)(t.loop_end - t.loop_start);
        const u32 head = (u32)(t.loop_start - t.first);
        assert(n2 == head + 2 * lap);
        /* The seam restarts the decoder a frame early; allow its tiny drift. */
        double err = 0;
        for (u32 i = 0; i < lap * ch; i++)
            err = fmax(err, fabs(twice[(head + lap) * ch + i] - twice[head * ch + i]));
        printf("loop [%d,%d): second lap max |diff| vs first = %.2e\n",
               t.loop_start - t.first, t.loop_end - t.first, err);
        assert(err < 1e-3);
        free(twice);
    }

    if (argc > 2) {
        FILE* w = fopen(argv[2], "wb");
        const u32 data = n1 * ch * 2, hdr[] = { 0x46464952, 36 + data, 0x45564157,
            0x20746D66, 16, 0x00010001u + (ch - 1) * 0x10000u, 48000, 48000 * ch * 2,
            (ch * 2) | (16u << 16), 0x61746164, data };
        fwrite(hdr, 4, 11, w);
        for (u32 i = 0; i < n1 * ch; i++) {
            float v = once[i] * 32767.0f;
            short s = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            fwrite(&s, 2, 1, w);
        }
        fclose(w);
    }
    free(once);
    puts("ok");
    return 0;
}

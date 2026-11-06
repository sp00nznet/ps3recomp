/*
 * cellAdec AC3, driven through its guest API.
 *
 * Built twice by CMakeLists.txt (adec_tests.cmake):
 *   without PS3RECOMP_FFMPEG_RUNTIME   mode "stub"
 *   with it                            modes "stub" (PS3RECOMP_FFMPEG=0) and "decode"
 * Modes:
 *   stub   AC3 AC3_2CH   AC3 5.1 frames give 1536 x 6 float of silence (36864 bytes in the PcmItem size, in
 *                        bsiInfo+0x14 and through cellAdecGetPcm), 2.0 frames 1536 x 2 (12288), codecType 5
 *                        the old 1024 x 2 (8192); AUDONE then PCMOUT for every AU.
 *   decode AC3 REF       every AU of a 5.1 AC3 stream through cellAdecDecodeAu; the PCM handed to the guest
 *                        (big-endian float, interleaved) must equal FFmpeg's own decode REF (f32le) sample
 *                        for sample. Negative controls: two channels swapped and one AU dropped must both
 *                        be caught.
 * The AC3 files are CBR 448 kb/s at 48 kHz, so every frame (= AU) is 1792 bytes (gen_adec_media.cmake).
 */
#include "../cellAdec.c"
#ifdef PS3RECOMP_FFMPEG_RUNTIME
#include "../ffmpeg_runtime.c"
#endif

#include <math.h>
#include <stdlib.h>

/* What the runtime would provide (the same stand-ins test_vdec_decode.c uses). */
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t ppu_hle_inject_base = 0x01000000u;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define MEM_SIZE   (64u << 20)
#define G_TYPE     0x10000u
#define G_CB       0x10010u
#define G_HANDLE   0x10020u
#define G_AUINFO   0x10040u
#define G_ITEMPTR  0x100A0u
#define G_AU       0x00100000u
#define G_OUT      0x00200000u
#define CB_OPD     0x00001000u
#define AC3_FRAME  1792u

static u32 s_msgs[8192];
static int s_nmsg;
static u32 s_handle;
static void fake_guest_call(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)a2; (void)a4; (void)a5; (void)a6; (void)a7;
    CHECK(opd == CB_OPD && a3 == 0xC0FFEEu && a0 == s_handle);
    /* AUDONE hands the AU back: a title's demuxer may refill it at once. */
    if ((u32)a1 == CELL_ADEC_MSG_TYPE_AUDONE) memset(vm_base + G_AU, 0xA5, 0x10000);
    if (s_nmsg < 8192) s_msgs[s_nmsg++] = (u32)a1;
}
ps3_guest_caller_fn g_ps3_guest_caller = fake_guest_call;

static u8* read_file(const char* path, size_t* len)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* p = (u8*)malloc((size_t)n + 1);
    CHECK(p && fread(p, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *len = (size_t)n;
    return p;
}

static void open_adec(u32 codec)
{
    vm_write32(G_TYPE, codec);
    vm_write32(G_CB, CB_OPD);
    vm_write32(G_CB + 4, 0xC0FFEEu);
    CHECK(cellAdecOpen((const CellAdecType*)(uintptr_t)G_TYPE, NULL,
                       (const CellAdecCb*)(uintptr_t)G_CB, (CellAdecHandle*)(uintptr_t)G_HANDLE) == CELL_OK);
    s_handle = vm_read32(G_HANDLE);
    CHECK(cellAdecStartSeq(s_handle, NULL) == CELL_OK);
}

/* One AU through the guest API: returns the PCM size the item announced and leaves the PCM at G_OUT. */
static u32 decode_one(const u8* au, u32 size)
{
    memcpy(vm_base + G_AU, au, size);
    vm_write32(G_AUINFO + 0, G_AU);
    vm_write32(G_AUINFO + 4, size);
    int before = s_nmsg;
    CHECK(cellAdecDecodeAu(s_handle, (const CellAdecAuInfo*)(uintptr_t)G_AUINFO) == CELL_OK);
    CHECK(s_nmsg == before + 2 && s_msgs[before] == CELL_ADEC_MSG_TYPE_AUDONE &&
          s_msgs[before + 1] == CELL_ADEC_MSG_TYPE_PCMOUT);
    CHECK(cellAdecGetPcmItem(s_handle, (const CellAdecPcmItem**)(uintptr_t)G_ITEMPTR) == CELL_OK);
    u32 item = vm_read32(G_ITEMPTR);
    u32 bytes = vm_read32(item + PCMITEM_SIZE);
    u32 bsi = vm_read32(item + PCMITEM_BSI_INFO);
    CHECK(bsi && vm_read32(bsi + 0x14) == bytes);
    CHECK(vm_read32(item + PCMITEM_AU_START) == G_AU && vm_read32(item + PCMITEM_AU_SIZE) == size);
    memset(vm_base + G_OUT, 0x5A, bytes + 64);
    CHECK(cellAdecGetPcm(s_handle, (void*)(uintptr_t)G_OUT) == CELL_OK);
    CHECK(vm_base[G_OUT + bytes] == 0x5A);           /* exactly `bytes` written */
    return bytes;
}

static int all_zero(u32 ea, u32 n)
{
    for (u32 i = 0; i < n; i++) if (vm_base[ea + i]) return 0;
    return 1;
}

static int mode_stub(int argc, char** argv)
{
    CHECK(argc >= 4);
    size_t n6, n2;
    u8* a6 = read_file(argv[2], &n6);
    u8* a2 = read_file(argv[3], &n2);
    CHECK(n6 >= AC3_FRAME && n2 >= AC3_FRAME && a6[0] == 0x0B && a6[1] == 0x77);
    CHECK(adec_ac3_channels(a6, AC3_FRAME) == 6 && adec_ac3_channels(a2, AC3_FRAME) == 2);
    open_adec(CELL_ADEC_TYPE_AC3);
    CHECK(s_adec[s_handle].ff == NULL);
    for (int i = 0; i < 3; i++) {
        u32 b = decode_one(a6 + (size_t)i * AC3_FRAME, AC3_FRAME);
        CHECK(b == 1536u * 6u * 4u && all_zero(G_OUT, b));
    }
    u32 b = decode_one(a2, AC3_FRAME);
    CHECK(b == 1536u * 2u * 4u && all_zero(G_OUT, b));
    CHECK(cellAdecClose(s_handle) == CELL_OK);
    open_adec(5);
    b = decode_one(a6, AC3_FRAME);
    CHECK(b == 1024u * 2u * 4u && all_zero(G_OUT, b));
    CHECK(cellAdecClose(s_handle) == CELL_OK);
    printf("OK stub: AC3 5.1 36864, 2.0 12288, codecType 5 8192 bytes of silence per AU\n");
    return 0;
}

#ifdef PS3RECOMP_FFMPEG_RUNTIME
/* Decode the whole stream; compare against REF. swap: exchange channels 0 and 1 of the guest PCM; drop: skip AU 3. */
static int run_decode(const u8* ac3, size_t n, const float* ref, size_t nref, int swap, int drop)
{
    open_adec(CELL_ADEC_TYPE_AC3);
    CHECK(s_adec[s_handle].ff != NULL);
    size_t at = 0;
    int bad = 0;
    for (size_t off = 0, k = 0; off + AC3_FRAME <= n; off += AC3_FRAME, k++) {
        if (drop && k == 3) continue;
        u32 b = decode_one(ac3 + off, AC3_FRAME);
        if (b != 1536u * 6u * 4u) { printf("AU %zu: %u bytes\n", k, b); bad = 1; break; }
        for (u32 i = 0; i < b / 4u; i++) {
            u32 idx = i;
            if (swap && (i % 6u) < 2u) idx = i ^ 1u;
            u32 w = vm_read32(G_OUT + idx * 4u);
            float v;
            memcpy(&v, &w, 4);
            if (at + i >= nref || v != ref[at + i]) {
                if (!bad) printf("sample %zu (AU %zu): %g vs %g\n", at + i, k, v, at + i < nref ? ref[at + i] : -1.f);
                bad = 1;
                break;
            }
        }
        if (bad) break;
        at += b / 4u;
    }
    if (!bad && at != nref) { printf("%zu samples, reference has %zu\n", at, nref); bad = 1; }
    CHECK(cellAdecClose(s_handle) == CELL_OK);
    return bad;
}

static int mode_decode(int argc, char** argv)
{
    CHECK(argc >= 4);
    if (!ps3_ffmpeg_get()) { printf("SKIP: libavcodec not loadable\n"); return 77; }
    size_t n, nr;
    u8* ac3 = read_file(argv[2], &n);
    u8* refb = read_file(argv[3], &nr);
    CHECK(n % AC3_FRAME == 0 && n / AC3_FRAME >= 8 && nr % 4 == 0);
    const float* ref = (const float*)refb;
    CHECK(run_decode(ac3, n, ref, nr / 4, 0, 0) == 0);
    /* Nonzero audio came through, not silence. */
    double e = 0;
    for (size_t i = 0; i < nr / 4; i++) e += fabs(ref[i]);
    CHECK(e > 1.0);
    printf("OK decode: %zu AUs, %zu samples equal FFmpeg's own decode\n", n / AC3_FRAME, nr / 4);
    CHECK(run_decode(ac3, n, ref, nr / 4, 1, 0) != 0);
    CHECK(run_decode(ac3, n, ref, nr / 4, 0, 1) != 0);
    printf("OK negative controls: swapped channels and a dropped AU are caught\n");
    return 0;
}
#endif

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = (uint8_t*)calloc(1, MEM_SIZE);
    CHECK(vm_base);
    ppu_vm_size = MEM_SIZE;
    const char* mode = argc > 1 ? argv[1] : "stub";
    if (strcmp(mode, "stub") == 0) return mode_stub(argc, argv);
#ifdef PS3RECOMP_FFMPEG_RUNTIME
    if (strcmp(mode, "decode") == 0) return mode_decode(argc, argv);
#endif
    printf("unknown mode %s\n", mode);
    return 2;
}

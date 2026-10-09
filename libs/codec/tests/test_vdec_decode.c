/*
 * ps3recomp -- cellVdec through the guest API, with and without libavcodec
 *
 * Built twice by CMakeLists.txt (PS3RECOMP_BUILD_TESTS):
 *   test_vdec_stub    no PS3RECOMP_FFMPEG_RUNTIME: the callback-only decoder
 *   test_vdec_ffmpeg  with it, against streams gen_vdec_media.cmake makes
 *
 * Modes:
 *   stub   [CODEC ES]      callback-only behaviour, exactly as before the
 *                          decoder existed: AUDONE + PICOUT per AU, a 1280x720
 *                          black picture, the old pic item. Run in the stub
 *                          build, and in the FFmpeg build with
 *                          PS3RECOMP_FFMPEG=0 or a missing library.
 *   decode CODEC ES REF W H
 *                          AU by AU through cellVdecDecodeAu; every picture
 *                          must equal FFmpeg's own decode (REF, I420), in order,
 *                          twice (EndSeq/StartSeq in between). Negative
 *                          controls: one changed byte and one dropped AU must
 *                          both be caught.
 *   rgba   CODEC ES REF W H
 *                          ARGB32 and RGBA32 output against FFmpeg's
 *                          (swscale) RGBA decode by PSNR; channel order
 *                          swapped must fail.
 * CODEC: avc | mpeg2.
 */
#include "../cellVdec.c"
#include "../vdec_ffmpeg.c"
#include "../ffmpeg_runtime.c"

#include <math.h>
#include <stdlib.h>

/* What the runtime would provide. */
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t ppu_hle_inject_base = 0x01000000u;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Guest layout */
#define MEM_SIZE   (64u << 20)
#define G_TYPE     0x10000u
#define G_CB       0x10010u
#define G_HANDLE   0x10020u
#define G_AUINFO   0x10040u
#define G_FMT      0x10080u
#define G_ITEMPTR  0x100A0u
#define G_AU       0x00100000u
#define G_PIC      0x00800000u
#define CB_OPD     0x00001000u

/* The guest callback: record the messages. */
static u32 s_msgs[4096];
static int s_nmsg;
static void fake_guest_call(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                            uint64_t a7)
{
    (void)a0; (void)a2; (void)a4; (void)a5; (void)a6; (void)a7;
    CHECK(opd == CB_OPD && a3 == 0xC0FFEEu);
    if (s_nmsg < 4096) s_msgs[s_nmsg++] = (u32)a1;
}
ps3_guest_caller_fn g_ps3_guest_caller = fake_guest_call;

static int count_msgs(u32 type)
{
    int n = 0;
    for (int i = 0; i < s_nmsg; i++) n += s_msgs[i] == type;
    return n;
}

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

/* ---------------------------------------------------------------------------
 * Splitting an elementary stream into access units
 * -----------------------------------------------------------------------*/
#define MAX_AU 256
static size_t s_au_off[MAX_AU + 1];
static int s_nau;

/* H.264: every AU starts with an access unit delimiter (NAL type 9).
 * MPEG-2: a picture start code (00 00 01 00) starts an AU, together with the
 * sequence / GOP headers in front of it. */
static void split(const u8* es, size_t len, int avc)
{
    s_nau = 0;
    long pending = -1;
    for (size_t i = 0; i + 3 < len; i++) {
        if (es[i] || es[i + 1] || es[i + 2] != 1) continue;
        u8 c = es[i + 3];
        size_t at = (i > 0 && es[i - 1] == 0) ? i - 1 : i;   /* 4-byte code */
        if (avc) {
            if ((c & 0x1F) == 9) { CHECK(s_nau < MAX_AU); s_au_off[s_nau++] = at; }
        } else if (c == 0xB3 || c == 0xB8) {
            if (pending < 0) pending = (long)i;
        } else if (c == 0x00) {
            CHECK(s_nau < MAX_AU);
            s_au_off[s_nau++] = pending >= 0 ? (size_t)pending : i;
            pending = -1;
        }
    }
    s_au_off[s_nau] = len;
    CHECK(s_nau > 0 && s_au_off[0] == 0);
}

/* ---------------------------------------------------------------------------
 * Driving cellVdec
 * -----------------------------------------------------------------------*/
static u32 open_decoder(u32 codec)
{
    vm_write32(G_TYPE, codec);
    vm_write32(G_TYPE + 4, 0);
    vm_write32(G_CB, CB_OPD);
    vm_write32(G_CB + 4, 0xC0FFEEu);
    CHECK(cellVdecOpen((const CellVdecType*)(uintptr_t)G_TYPE, NULL,
                       (const CellVdecCb*)(uintptr_t)G_CB,
                       (CellVdecHandle*)(uintptr_t)G_HANDLE) == CELL_OK);
    return vm_read32(G_HANDLE);
}

static u64 au_pts(int i) { return 90000ull + 3600ull * (u64)i; }

static s32 submit(u32 h, const u8* es, int i)
{
    u32 size = (u32)(s_au_off[i + 1] - s_au_off[i]);
    memcpy(vm_base + G_AU, es + s_au_off[i], size);
    vm_write32(G_AUINFO + 0x00, G_AU);
    vm_write32(G_AUINFO + 0x04, size);
    vm_write64(G_AUINFO + 0x08, au_pts(i));
    vm_write64(G_AUINFO + 0x10, (u64)i);                    /* dts */
    vm_write64(G_AUINFO + 0x18, 0xABCD0000ull + (u64)i);    /* userData */
    return cellVdecDecodeAu(h, 0, (const CellVdecAuInfo*)(uintptr_t)G_AUINFO);
}

static void set_format(u32 type, u32 matrix, u8 alpha)
{
    vm_write32(G_FMT + 0, type);
    vm_write32(G_FMT + 4, matrix);
    vm_write8(G_FMT + 8, alpha);
}

/* Pictures taken so far in this run, as GetPicItem described them. */
typedef struct { u64 pts, dts, user; u32 w, h, type, idr; } Item;
static Item s_items[MAX_AU * 2];
static int s_npic;
static u8* s_frames;            /* s_npic pictures, frame_size each */
static size_t s_frame_size;

static void take_pictures(u32 h, u32 codec, u32 fmt_type)
{
    for (;;) {
        s32 r = cellVdecGetPicItem(h, (const CellVdecPicItem**)(uintptr_t)G_ITEMPTR);
        if (r == (s32)CELL_VDEC_ERROR_EMPTY) return;
        CHECK(r == CELL_OK);
        u32 it = vm_read32(G_ITEMPTR), info = vm_read32(it + 0x48);
        Item* x = &s_items[s_npic];
        x->pts  = ((u64)vm_read32(it + 0x10) << 32) | vm_read32(it + 0x14);
        x->dts  = ((u64)vm_read32(it + 0x20) << 32) | vm_read32(it + 0x24);
        x->user = vm_read64(it + 0x30);
        x->w = vm_read16(info + 0);
        x->h = vm_read16(info + 2);
        x->type = codec == CELL_VDEC_CODEC_TYPE_AVC ? vm_read8(info + 4) : vm_read8(info + 0x12);
        x->idr = vm_read8(info + 6);
        CHECK(vm_read32(it + 0x00) == codec);
        set_format(fmt_type, 0, 0xFF);
        CHECK(cellVdecGetPicture(h, (const void*)(uintptr_t)G_FMT,
                                 (void*)(uintptr_t)G_PIC) == CELL_OK);
        CHECK((size_t)(s_npic + 1) * s_frame_size <= (size_t)MAX_AU * 2 * s_frame_size);
        memcpy(s_frames + (size_t)s_npic * s_frame_size, vm_base + G_PIC, s_frame_size);
        s_npic++;
    }
}

/* One whole sequence: StartSeq, every AU except `drop`, EndSeq. */
static void run_sequence(u32 h, u32 codec, const u8* es, int drop, u32 fmt_type)
{
    s_npic = 0;
    s_nmsg = 0;
    CHECK(cellVdecStartSeq(h) == CELL_OK);
    for (int i = 0; i < s_nau; i++) {
        if (i == drop) continue;
        CHECK(submit(h, es, i) == CELL_OK);
        take_pictures(h, codec, fmt_type);
    }
    int before = count_msgs(CELL_VDEC_MSG_TYPE_PICOUT);
    CHECK(cellVdecEndSeq(h) == CELL_OK);
    /* Pictures held for reordering come out on EndSeq, before SEQDONE. */
    CHECK(s_nmsg > 0 && s_msgs[s_nmsg - 1] == CELL_VDEC_MSG_TYPE_SEQDONE);
    CHECK(count_msgs(CELL_VDEC_MSG_TYPE_PICOUT) > before);
    take_pictures(h, codec, fmt_type);
    CHECK(count_msgs(CELL_VDEC_MSG_TYPE_AUDONE) == s_nau - (drop >= 0));
    CHECK(count_msgs(CELL_VDEC_MSG_TYPE_PICOUT) == s_npic);
}

static int count_mismatches(const u8* ref, int nref)
{
    int bad = abs(nref - s_npic);
    for (int i = 0; i < s_npic && i < nref; i++)
        bad += memcmp(s_frames + (size_t)i * s_frame_size,
                      ref + (size_t)i * s_frame_size, s_frame_size) != 0;
    return bad;
}

static u32 parse_codec(const char* s)
{
    return strcmp(s, "avc") == 0 ? CELL_VDEC_CODEC_TYPE_AVC : CELL_VDEC_CODEC_TYPE_MPEG2;
}

/* ---------------------------------------------------------------------------
 * Modes
 * -----------------------------------------------------------------------*/

/* Exactly what cellVdec did before it had a decoder. */
static int mode_stub(int argc, char** argv)
{
    u32 codec = argc > 2 ? parse_codec(argv[2]) : CELL_VDEC_CODEC_TYPE_AVC;
    size_t len = 0;
    u8* es = NULL;
    if (argc > 3) { es = read_file(argv[3], &len); split(es, len, codec == CELL_VDEC_CODEC_TYPE_AVC); }
    else { static u8 junk[64 * 8]; es = junk; len = sizeof(junk);
           s_nau = 8; for (int i = 0; i <= 8; i++) s_au_off[i] = (size_t)i * 64; }

    u32 h = open_decoder(codec);
    CHECK(s_vdec[h].ff == NULL);
    s_nmsg = 0;
    CHECK(cellVdecStartSeq(h) == CELL_OK);
    for (int i = 0; i < 3 && i < s_nau; i++) {
        int m0 = s_nmsg;
        CHECK(submit(h, es, i) == CELL_OK);
        CHECK(s_nmsg == m0 + 2 && s_msgs[m0] == CELL_VDEC_MSG_TYPE_AUDONE
              && s_msgs[m0 + 1] == CELL_VDEC_MSG_TYPE_PICOUT);
        CHECK(cellVdecGetPicItem(h, (const CellVdecPicItem**)(uintptr_t)G_ITEMPTR) == CELL_OK);
        u32 it = vm_read32(G_ITEMPTR);
        CHECK(it == VDEC_ITEM_EA(h));
        CHECK(vm_read32(it + 0x00) == codec);
        CHECK(vm_read32(it + 0x04) == G_AU);
        CHECK(vm_read32(it + 0x08) == (u32)(s_au_off[i + 1] - s_au_off[i]));
        CHECK(vm_read8(it + 0x0C) == 1);
        CHECK(vm_read32(it + 0x10) == (u32)(au_pts(i) >> 32) && vm_read32(it + 0x14) == (u32)au_pts(i));
        CHECK(vm_read32(it + 0x18) == 0xFFFFFFFFu && vm_read32(it + 0x1C) == 0xFFFFFFFFu);
        CHECK(vm_read64(it + 0x30) == 0xABCD0000ull + (u64)i);
        CHECK(vm_read32(it + 0x48) == VDEC_INFO_EA(h));
        CHECK(vm_read16(it + 0x80) == 1280 && vm_read16(it + 0x82) == 720);
        for (u32 o = 0x84; o < 0x100; o++) CHECK(vm_read8(it + o) == 0);
        /* GetPictureExt never existed without a decoder: CELL_OK, nothing done. */
        CHECK(cellVdecGetPictureExt(h, (const void*)(uintptr_t)G_FMT,
                                    (void*)(uintptr_t)G_PIC, 0) == CELL_OK);
        set_format(3, 0, 0);
        memset(vm_base + G_PIC, 0x55, 1280 * 720 * 3 / 2 + 16);
        CHECK(cellVdecGetPicture(h, (const void*)(uintptr_t)G_FMT, (void*)(uintptr_t)G_PIC) == CELL_OK);
        for (u32 o = 0; o < 1280 * 720; o += 997) CHECK(vm_base[G_PIC + o] == 0x10);
        for (u32 o = 1280 * 720; o < 1280 * 720 * 3 / 2; o += 331) CHECK(vm_base[G_PIC + o] == 0x80);
        CHECK(vm_base[G_PIC + 1280 * 720 * 3 / 2] == 0x55);
        CHECK(cellVdecGetPicture(h, (const void*)(uintptr_t)G_FMT, (void*)(uintptr_t)G_PIC)
              == (s32)CELL_VDEC_ERROR_EMPTY);
    }
    int m0 = s_nmsg;
    CHECK(cellVdecEndSeq(h) == CELL_OK);
    CHECK(s_nmsg == m0 + 1 && s_msgs[m0] == CELL_VDEC_MSG_TYPE_SEQDONE);
    CHECK(cellVdecClose(h) == CELL_OK);
    printf("stub: callback-only behaviour unchanged (%d AUs)\n", s_nau < 3 ? s_nau : 3);
    return 0;
}

static int mode_decode(int argc, char** argv)
{
    CHECK(argc == 7);
    u32 codec = parse_codec(argv[2]);
    u32 W = (u32)atoi(argv[5]), H = (u32)atoi(argv[6]);
    size_t len, rlen;
    u8* es = read_file(argv[3], &len);
    u8* ref = read_file(argv[4], &rlen);
    split(es, len, codec == CELL_VDEC_CODEC_TYPE_AVC);
    s_frame_size = (size_t)W * H * 3 / 2;
    int nref = (int)(rlen / s_frame_size);
    CHECK(nref > 0 && rlen == (size_t)nref * s_frame_size);
    s_frames = (u8*)malloc((size_t)MAX_AU * 2 * s_frame_size);

    u32 h = open_decoder(codec);
    if (!s_vdec[h].ff) {
        /* No loadable libavcodec here (not installed, or not on the loader
         * path): a skip, not a failure. A library that loads but cannot open
         * the decoder is still a failure. */
#ifdef PS3RECOMP_FFMPEG_RUNTIME
        if (!ps3_ffmpeg_get()) { printf("SKIP: libavcodec not loadable\n"); return 77; }
#endif
        printf("FAIL: no libavcodec decoder\n");
        return 1;
    }
    printf("%s: %d AUs, %d reference frames %ux%u\n", argv[2], s_nau, nref, W, H);

    for (int pass = 0; pass < 2; pass++) {       /* the second after a seq reset */
        run_sequence(h, codec, es, -1, 3);
        int bad = count_mismatches(ref, nref);
        printf("pass %d: %d pictures, %d differ from ffmpeg's decode\n", pass, s_npic, bad);
        CHECK(bad == 0);
        int types[4] = {0}, idr = 0, reordered = 0;
        u64 seen = 0;
        for (int i = 0; i < s_npic; i++) {
            const Item* x = &s_items[i];
            CHECK(x->w == W && x->h == H);
            int k = (int)((x->pts - 90000ull) / 3600ull);   /* the AU it came from */
            CHECK(k >= 0 && k < s_nau && au_pts(k) == x->pts);
            CHECK(x->dts == (u64)k && x->user == 0xABCD0000ull + (u64)k);
            CHECK(!(seen & (1ull << k)));
            seen |= 1ull << k;
            reordered += k != i;
            if (codec == CELL_VDEC_CODEC_TYPE_AVC) { CHECK(x->type <= 2); types[x->type + 1]++; idr += x->idr; }
            else { CHECK(x->type >= 1 && x->type <= 3); types[x->type]++; }
        }
        printf("  I/P/B %d/%d/%d, %d IDR, %d pictures out of AU order\n",
               types[1], types[2], types[3], idr, reordered);
        CHECK(types[1] > 0 && types[2] > 0 && types[3] > 0 && reordered > 0);
        if (codec == CELL_VDEC_CODEC_TYPE_AVC) CHECK(idr > 0);
    }

    /* Negative control 1: the comparison sees a single changed byte. */
    s_frames[s_frame_size * 3 + 123] ^= 1;
    CHECK(count_mismatches(ref, nref) == 1);
    /* Negative control 2: decoding without one P/B AU differs from the reference. */
    run_sequence(h, codec, es, 4, 3);
    int bad = count_mismatches(ref, nref);
    printf("negative control: AU 4 dropped -> %d mismatches\n", bad);
    CHECK(bad > 0);

    CHECK(cellVdecClose(h) == CELL_OK);
    printf("decode: OK\n");
    return 0;
}

static double psnr(const u8* a, const u8* b, size_t n, int skip_byte)
{
    double se = 0; size_t cnt = 0;
    for (size_t i = 0; i < n; i++) {
        if ((int)(i % 4) == skip_byte) continue;
        double d = (double)a[i] - (double)b[i];
        se += d * d; cnt++;
    }
    return se == 0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / (se / (double)cnt));
}

static int mode_rgba(int argc, char** argv)
{
    CHECK(argc == 7);
    u32 codec = parse_codec(argv[2]);
    u32 W = (u32)atoi(argv[5]), H = (u32)atoi(argv[6]);
    size_t len, rlen;
    u8* es = read_file(argv[3], &len);
    u8* ref = read_file(argv[4], &rlen);
    split(es, len, codec == CELL_VDEC_CODEC_TYPE_AVC);
    s_frame_size = (size_t)W * H * 4;
    int nref = (int)(rlen / s_frame_size);
    s_frames = (u8*)malloc((size_t)MAX_AU * 2 * s_frame_size);
    u32 h = open_decoder(codec);
#ifdef PS3RECOMP_FFMPEG_RUNTIME
    if (!s_vdec[h].ff && !ps3_ffmpeg_get()) { printf("SKIP: libavcodec not loadable\n"); return 77; }
#endif
    CHECK(s_vdec[h].ff);

    /* RGBA32: bytes R G B A. */
    run_sequence(h, codec, es, -1, 1);
    CHECK(s_npic == nref);
    double worst = 99;
    for (int i = 0; i < s_npic; i++) {
        const u8* p = s_frames + (size_t)i * s_frame_size;
        for (size_t o = 3; o < s_frame_size; o += 4) CHECK(p[o] == 0xFF);
        double q = psnr(p, ref + (size_t)i * s_frame_size, s_frame_size, 3);
        if (q < worst) worst = q;
    }
    printf("RGBA32: %d pictures, worst PSNR %.2f dB against swscale\n", s_npic, worst);
    CHECK(worst > 35.0);

    /* ARGB32: bytes A R G B, so shifted by one against the reference. */
    run_sequence(h, codec, es, -1, 0);
    double worst_argb = 99, wrong_order = 0;
    for (int i = 0; i < s_npic; i++) {
        u8* p = s_frames + (size_t)i * s_frame_size;
        for (size_t o = 0; o < s_frame_size; o += 4) {
            CHECK(p[o] == 0xFF);
            u8 a = p[o]; p[o] = p[o + 1]; p[o + 1] = p[o + 2]; p[o + 2] = p[o + 3]; p[o + 3] = a;
        }
        double q = psnr(p, ref + (size_t)i * s_frame_size, s_frame_size, 3);
        if (q < worst_argb) worst_argb = q;
        /* Negative control: R and B swapped must not pass. */
        for (size_t o = 0; o < s_frame_size; o += 4) { u8 t = p[o]; p[o] = p[o + 2]; p[o + 2] = t; }
        wrong_order += psnr(p, ref + (size_t)i * s_frame_size, s_frame_size, 3) / s_npic;
    }
    printf("ARGB32: worst PSNR %.2f dB; R/B swapped averages %.2f dB\n", worst_argb, wrong_order);
    CHECK(worst_argb > 35.0 && wrong_order < 25.0);
    CHECK(cellVdecClose(h) == CELL_OK);
    printf("rgba: OK\n");
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = (uint8_t*)calloc(1, MEM_SIZE);
    CHECK(vm_base);
    ppu_vm_size = MEM_SIZE;
    const char* mode = argc > 1 ? argv[1] : "stub";
    if (strcmp(mode, "stub") == 0)   return mode_stub(argc, argv);
    if (strcmp(mode, "decode") == 0) return mode_decode(argc, argv);
    if (strcmp(mode, "rgba") == 0)   return mode_rgba(argc, argv);
    printf("unknown mode %s\n", mode);
    return 2;
}

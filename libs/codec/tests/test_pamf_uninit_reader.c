/*
 * test_pamf_uninit_reader - cellPamf calls on a reader that was never
 * successfully initialised.
 *
 * A title can ask a PAMF reader for its presentation start time before
 * cellPamfReaderInitialize has run on it, for example when its movie never
 * opened. The reader table handed such an EA a fresh, zeroed slot, and the
 * getter then read the header through its NULL pamfAddr (an access violation
 * at NULL+0x18 for GetPresentationStartTime). Every call except Initialize
 * now looks the reader up instead, and an unknown or failed reader is
 * CELL_PAMF_ERROR_INVALID_ARG.
 *
 * Checks, all through guest EAs:
 *   - each reader call on an EA Initialize never saw fails cleanly and
 *     leaves its output untouched;
 *   - a reader whose Initialize failed (bad magic) stays invalid;
 *   - a reader initialised from a well-formed header still answers.
 *
 *   cc -std=gnu17 -I include -o test_pamf_uninit_reader libs/codec/tests/test_pamf_uninit_reader.c
 * Driven by tools/test_pamf_uninit_reader.py.
 */
#include "../cellPamf.c"
#include <stdlib.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           fflush(stdout); if (!(c)) fails++; } while (0)

#define INVALID ((s32)CELL_PAMF_ERROR_INVALID_ARG)
#define EA(x)   ((CellPamfReader*)(uintptr_t)(x))
#define OUT(x)  ((void*)(uintptr_t)(x))

static const u32 READER_NEW  = 0x1000;   /* never initialised */
static const u32 READER_BAD  = 0x1080;   /* Initialize failed */
static const u32 READER_GOOD = 0x1100;   /* initialised */
static const u32 OUT_EA      = 0x2000;   /* output scratch */
static const u32 HDR_EA      = 0x10000;  /* PAMF header */
static const u32 BAD_EA      = 0x20000;  /* header with the wrong magic */

static void put32(u32 ea, u32 v) { vm_write32(ea, v); }
static void put16(u32 ea, u16 v) { vm_base[ea] = (u8)(v >> 8); vm_base[ea + 1] = (u8)v; }

/* A header in the layout cellPamf.c parses: one AVC stream, one entry point. */
static void build_header(void)
{
    put32(HDR_EA + PAMF_OFF_MAGIC,         CELL_PAMF_MAGIC);
    put32(HDR_EA + PAMF_OFF_VERSION,       0x00040100);
    put32(HDR_EA + PAMF_OFF_DATA_OFFSET,   0x800);
    put32(HDR_EA + PAMF_OFF_DATA_SIZE,     0);
    put32(HDR_EA + PAMF_OFF_STREAM_TABLE,  0x30);
    put16(HDR_EA + PAMF_OFF_NUM_STREAMS,   1);
    put32(HDR_EA + PAMF_OFF_PRESENT_START, 0x1234);
    put32(HDR_EA + PAMF_OFF_PRESENT_END,   0x5678);
    put32(HDR_EA + PAMF_OFF_MUX_RATE,      0x9ABC);
    put32(HDR_EA + PAMF_OFF_EP_OFFSET,     0x60);
    put32(HDR_EA + PAMF_OFF_EP_COUNT,      1);
    vm_base[HDR_EA + 0x30] = PAMF_RAW_TYPE_AVC;
    put32(HDR_EA + 0x60, 0x11);
    put32(HDR_EA + 0x64, 0x22);
    memcpy(vm_base + BAD_EA, vm_base + HDR_EA, 0x80);
    put32(BAD_EA + PAMF_OFF_MAGIC, 0x4E4F5045);  /* "NOPE" */
}

static void poison_out(void) { memset(vm_base + OUT_EA, 0xAA, 0x100); }
static int out_untouched(void)
{
    for (int i = 0; i < 0x100; i++)
        if (vm_base[OUT_EA + i] != 0xAA) return 0;
    return 1;
}

static void expect_invalid(u32 reader, const char* what)
{
    s32 r;
    poison_out();
    r = cellPamfReaderGetPresentationStartTime(EA(reader), OUT(OUT_EA));
    CHECK(r == INVALID && out_untouched(), "%s: GetPresentationStartTime -> 0x%08X", what, (u32)r);
    poison_out();
    r = cellPamfReaderGetPresentationEndTime(EA(reader), OUT(OUT_EA));
    CHECK(r == INVALID && out_untouched(), "%s: GetPresentationEndTime -> 0x%08X", what, (u32)r);
    u32 mux = cellPamfReaderGetMuxRateBound(EA(reader));
    CHECK(mux == 0, "%s: GetMuxRateBound -> %u", what, mux);
    r = cellPamfReaderGetNumberOfStreams(EA(reader));
    CHECK(r == INVALID, "%s: GetNumberOfStreams -> 0x%08X", what, (u32)r);
    r = cellPamfReaderGetNumberOfSpecificStreams(EA(reader), CELL_PAMF_STREAM_TYPE_AVC);
    CHECK(r == INVALID, "%s: GetNumberOfSpecificStreams -> 0x%08X", what, (u32)r);
    r = cellPamfReaderSetStreamWithType(EA(reader), CELL_PAMF_STREAM_TYPE_AVC, 0);
    CHECK(r == INVALID, "%s: SetStreamWithType -> 0x%08X", what, (u32)r);
    r = cellPamfReaderSetStreamWithTypeAndChannel(EA(reader), CELL_PAMF_STREAM_TYPE_AVC, 0);
    CHECK(r == INVALID, "%s: SetStreamWithTypeAndChannel -> 0x%08X", what, (u32)r);
    r = cellPamfReaderSetStreamWithIndex(EA(reader), 0);
    CHECK(r == INVALID, "%s: SetStreamWithIndex -> 0x%08X", what, (u32)r);
    r = cellPamfReaderGetStreamIndex(EA(reader));
    CHECK(r == INVALID, "%s: GetStreamIndex -> 0x%08X", what, (u32)r);
    poison_out();
    r = cellPamfReaderGetStreamInfo(EA(reader), OUT(OUT_EA), 0x40);
    CHECK(r == INVALID && out_untouched(), "%s: GetStreamInfo -> 0x%08X", what, (u32)r);
    poison_out();
    r = cellPamfReaderGetNumberOfEp(EA(reader), OUT(OUT_EA));
    CHECK(r == INVALID && out_untouched(), "%s: GetNumberOfEp -> 0x%08X", what, (u32)r);
    poison_out();
    r = cellPamfReaderGetEp(EA(reader), 0, OUT(OUT_EA));
    CHECK(r == INVALID && out_untouched(), "%s: GetEp -> 0x%08X", what, (u32)r);
}

int main(void)
{
    ppu_vm_size = 0x40000;
    vm_base = calloc(1, ppu_vm_size);
    if (!vm_base) return 2;
    build_header();

    /* Before any Initialize at all. */
    expect_invalid(READER_NEW, "never initialised");

    /* A failed Initialize leaves that reader invalid. */
    s32 r = cellPamfReaderInitialize(EA(READER_BAD), OUT(BAD_EA), 0x800, 0);
    CHECK(r == (s32)CELL_PAMF_ERROR_INVALID_HEADER, "bad magic: Initialize -> 0x%08X", (u32)r);
    expect_invalid(READER_BAD, "failed Initialize");

    /* A good reader still works, and the others stay invalid beside it. */
    r = cellPamfReaderInitialize(EA(READER_GOOD), OUT(HDR_EA), 0x800, 0);
    CHECK(r == CELL_OK, "good header: Initialize -> 0x%08X", (u32)r);
    r = cellPamfReaderGetPresentationStartTime(EA(READER_GOOD), OUT(OUT_EA));
    CHECK(r == CELL_OK && vm_read64(OUT_EA) == 0x1234, "initialised: start time 0x%llX",
          (unsigned long long)vm_read64(OUT_EA));
    r = cellPamfReaderGetPresentationEndTime(EA(READER_GOOD), OUT(OUT_EA));
    CHECK(r == CELL_OK && vm_read64(OUT_EA) == 0x5678, "initialised: end time 0x%llX",
          (unsigned long long)vm_read64(OUT_EA));
    CHECK(cellPamfReaderGetMuxRateBound(EA(READER_GOOD)) == 0x9ABC, "initialised: mux rate");
    CHECK(cellPamfReaderGetNumberOfStreams(EA(READER_GOOD)) == 1, "initialised: one stream");
    r = cellPamfReaderGetNumberOfEp(EA(READER_GOOD), OUT(OUT_EA));
    CHECK(r == CELL_OK && vm_read32(OUT_EA) == 1, "initialised: one entry point");
    r = cellPamfReaderGetEp(EA(READER_GOOD), 0, OUT(OUT_EA));
    CHECK(r == CELL_OK && vm_read32(OUT_EA) == 0x11 && vm_read32(OUT_EA + 4) == 0x22,
          "initialised: entry point 0 is (0x%X, 0x%X)", vm_read32(OUT_EA), vm_read32(OUT_EA + 4));
    expect_invalid(READER_NEW, "never initialised, beside a good reader");

    printf(fails ? "FAIL: %d check(s)\n" : "PASS: cellPamf reader calls on uninitialised readers\n", fails);
    return fails ? 1 : 0;
}

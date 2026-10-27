/* cellSpursQueueSize against the real libs/spurs/cellSpurs.c: the occupancy comes from the ring indices
 * (W0 pop, W1 push, both normalised, modulo 2*W3), the model cellSpursQueuePushBody uses. The queue words are
 * written big-endian into guest memory exactly as the guest would see them; no copy of the algorithm here. */
#include <ps3emu/ps3types.h>

/* Weak on purpose: where the export is missing (upstream master before the fix) the test fails with a message
 * instead of a link error. */
extern s32 cellSpursQueueSize(u64 queue_ea, u64 size_ea) __attribute__((weak));

#include "../libs/spurs/cellSpurs.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size=0x20000;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
int g_resv_store_active;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void sys_event_queue_cancel_by_id(u32 id) { (void)id; }
void sys_event_queue_uncancel_by_id(u32 id) { (void)id; }


#define Q    0x1000u   /* 128-byte aligned queue */
#define OUT  0x2000u

static void ring(int32_t w0, int32_t w1, int32_t w3, int32_t depth)
{
    memset(vm_base + Q, 0, 0x80);
    vm_write32(Q + 0x00, (uint32_t)w0);
    vm_write32(Q + 0x04, (uint32_t)w1);
    vm_write32(Q + 0x0C, (uint32_t)w3);
    vm_write32(Q + 0x14, (uint32_t)depth);
}
static uint32_t size_of(void)
{
    vm_write32(OUT, 0xDEADBEEFu);   /* stale stack value, what the unresolved import left behind */
    assert(cellSpursQueueSize(Q, OUT) == CELL_OK);
    return vm_read32(OUT);
}
#define CHECK(w0, w1, w3, d, want) do { ring(w0, w1, w3, d); uint32_t got = size_of(); \
    if (got != (uint32_t)(want)) { fprintf(stderr, "FAIL line %d: ring(%d,%d,%d,%d) size %u, want %u\n", \
        __LINE__, (int)(w0), (int)(w1), (int)(w3), (int)(d), got, (unsigned)(want)); exit(1); } } while (0)

int main(void)
{
    if (!cellSpursQueueSize) { fprintf(stderr, "FAIL: cellSpursQueueSize is not defined (no handler: the import is unresolved)\n"); return 1; }
    vm_base = calloc(1, 0x10000); assert(vm_base);

    CHECK(0, 0, 8, 8, 0);          /* empty */
    CHECK(0, 3, 8, 8, 3);          /* three pushed */
    CHECK(2, 5, 8, 8, 3);          /* pop advanced as well */
    CHECK(0, 8, 8, 8, 8);          /* full (depth entries) */
    CHECK(10, 3, 8, 8, 9);         /* push wrapped past pop: 3 - 10 + 16 */
    CHECK(~5, ~9, 8, 8, 4);        /* negative (normalised) indices: ~w for w < 0 */
    CHECK(5, ~9, 8, 8, 4);         /* mixed */

    /* errors, and *size untouched on them */
    vm_write32(OUT, 0x12345678u);
    assert(cellSpursQueueSize(0, OUT) == (s32)CELL_SPURS_TASK_ERROR_NULL_POINTER);
    assert(cellSpursQueueSize(Q, 0) == (s32)CELL_SPURS_TASK_ERROR_NULL_POINTER);
    assert(cellSpursQueueSize(Q + 0x10, OUT) == (s32)CELL_SPURS_TASK_ERROR_ALIGN);
    ring(0, 0, 0, 0);              /* never initialised: no depth at +0x0C */
    assert(cellSpursQueueSize(Q, OUT) == (s32)CELL_SPURS_TASK_ERROR_INVAL);
    ring(0, 3, 0, 8);              /* a word at +0x14 is not a depth (it is part of the buffer EA) */
    assert(cellSpursQueueSize(Q, OUT) == (s32)CELL_SPURS_TASK_ERROR_INVAL);
    assert(vm_read32(OUT) == 0x12345678u);

    free(vm_base);
    puts("PASS: cellSpursQueueSize occupancy, wrap, normalised indices, NULL/alignment/uninitialised errors");
    return 0;
}

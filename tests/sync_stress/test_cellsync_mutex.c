/* cellSyncMutex: a CellSyncMutex that is not 4-byte aligned is
 * CELL_SYNC_ERROR_ALIGN from every entry point, and nothing is written to it;
 * an aligned one locks, reports busy and unlocks as a ticket lock should.
 *
 * Links libs/sync/cellSync.c only; the SPU lock-line hooks it calls around
 * the shared word are stubbed. */
#include "libs/sync/cellSync.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
uint32_t g_ww_lo, g_ww_hi;                    /* write-watch window (ppu_memory.h) */
int g_resv_store_active;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea, uint64_t v, int size) { (void)ea; (void)v; (void)size; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
int  spu_coh_is_reserved(uint32_t ea) { (void)ea; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }

static int failures;
#define CHECK_RC(call, want) do { int32_t _rc = (int32_t)(call); \
    if (_rc != (int32_t)(want)) { fprintf(stderr, "FAIL line %d: %s -> 0x%08X, want 0x%08X\n", \
        __LINE__, #call, (unsigned)_rc, (unsigned)(want)); failures++; } } while (0)
#define M(ea) ((CellSyncMutex*)(uintptr_t)(ea))

int main(void)
{
    vm_base = (uint8_t*)malloc(0x1000);
    if (!vm_base) return 2;

    /* Misaligned: ALIGN, and the bytes around it are not touched. */
    for (uint32_t ea = 0x101; ea <= 0x103; ea++) {
        memset(vm_base, 0xA5, 0x1000);
        CHECK_RC(cellSyncMutexInitialize(M(ea)), CELL_SYNC_ERROR_ALIGN);
        CHECK_RC(cellSyncMutexLock(M(ea)), CELL_SYNC_ERROR_ALIGN);
        CHECK_RC(cellSyncMutexTryLock(M(ea)), CELL_SYNC_ERROR_ALIGN);
        CHECK_RC(cellSyncMutexUnlock(M(ea)), CELL_SYNC_ERROR_ALIGN);
        for (uint32_t i = 0; i < 0x1000; i++)
            if (vm_base[i] != 0xA5) { fprintf(stderr, "FAIL: byte 0x%X changed by a misaligned call at 0x%X\n", i, ea); failures++; break; }
    }
    CHECK_RC(cellSyncMutexInitialize(NULL), CELL_SYNC_ERROR_NULL_POINTER);

    /* Aligned: the ticket lock works. */
    CHECK_RC(cellSyncMutexInitialize(M(0x100)), CELL_OK);
    CHECK_RC(cellSyncMutexLock(M(0x100)), CELL_OK);
    CHECK_RC(cellSyncMutexTryLock(M(0x100)), CELL_SYNC_ERROR_BUSY);
    CHECK_RC(cellSyncMutexUnlock(M(0x100)), CELL_OK);
    CHECK_RC(cellSyncMutexTryLock(M(0x100)), CELL_OK);
    CHECK_RC(cellSyncMutexUnlock(M(0x100)), CELL_OK);

    free(vm_base);
    printf("cellSyncMutex alignment: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

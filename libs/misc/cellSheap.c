/*
 * ps3recomp - cellSheap HLE implementation
 *
 * Simple bump allocator backed by a user-supplied buffer.
 * Uses a block tracking table for free/query support.
 */

#include "cellSheap.h"
#include <stdio.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */

/* Internal state */

typedef struct {
    u32 ea;
    u32 size;
} SheapBlock;

typedef struct {
    int in_use;
    u32 base_ea;
    u32 totalSize;
    u32 align;
    u32 offset;     /* bump pointer offset from base_ea */
    SheapBlock blocks[CELL_SHEAP_BLOCK_MAX];
    int blockCount;
} SheapState;

static SheapState s_heaps[CELL_SHEAP_MAX];

static u32 align_up(u32 val, u32 alignment)
{
    return (val + alignment - 1) & ~(alignment - 1);
}

/* API */

s32 cellSheapInitialize(const CellSheapAttr* attr, CellSheapHandle* handle)
{
    printf("[cellSheap] Initialize()\n");

    /* CellSheapAttr is { void* heapBase; u32 heapSize; u32 align; ... } --
     * the base is a 4-byte EA to the guest and 8 bytes to us, so every
     * field after it is at a different offset. Read by guest offset. */
    u32 attr_ea   = GUEST_EA(attr);
    u32 heap_base = attr_ea ? vm_read32(attr_ea + 0) : 0;
    u32 heap_size = attr_ea ? vm_read32(attr_ea + 4) : 0;
    u32 attr_algn = attr_ea ? vm_read32(attr_ea + 8) : 0;

    if (!attr || !handle || !heap_base || heap_size == 0)
        return (s32)CELL_SHEAP_ERROR_INVAL;

    u32 alignment = attr_algn;
    if (alignment == 0) alignment = 16;
    if (alignment & (alignment - 1))
        return (s32)CELL_SHEAP_ERROR_ALIGN;

    for (int i = 0; i < CELL_SHEAP_MAX; i++) {
        if (!s_heaps[i].in_use) {
            memset(&s_heaps[i], 0, sizeof(SheapState));
            s_heaps[i].in_use = 1;
            s_heaps[i].base_ea = heap_base;
            s_heaps[i].totalSize = heap_size;
            s_heaps[i].align = alignment;
            s_heaps[i].offset = 0;
            vm_write32((u32)(uintptr_t)handle, (u32)i);
            return CELL_OK;
        }
    }
    return (s32)CELL_SHEAP_ERROR_NOMEM;
}

s32 cellSheapFinalize(CellSheapHandle handle)
{
    printf("[cellSheap] Finalize(%u)\n", handle);
    if (handle >= CELL_SHEAP_MAX || !s_heaps[handle].in_use)
        return (s32)CELL_SHEAP_ERROR_INVAL;
    s_heaps[handle].in_use = 0;
    return CELL_OK;
}

void* cellSheapAllocate(CellSheapHandle handle, u32 size)
{
    if (handle >= CELL_SHEAP_MAX || !s_heaps[handle].in_use)
        return NULL;

    SheapState* h = &s_heaps[handle];
    u32 aligned_offset = align_up(h->offset, h->align);
    u32 aligned_size = align_up(size, h->align);

    if (aligned_offset + aligned_size > h->totalSize)
        return NULL;

    if (h->blockCount >= CELL_SHEAP_BLOCK_MAX)
        return NULL;

    u32 block_ea = h->base_ea + aligned_offset;
    h->blocks[h->blockCount].ea = block_ea;
    h->blocks[h->blockCount].size = aligned_size;
    h->blockCount++;
    h->offset = aligned_offset + aligned_size;

    return (void*)(uintptr_t)block_ea;
}

s32 cellSheapFree(CellSheapHandle handle, void* ptr)
{
    if (handle >= CELL_SHEAP_MAX || !s_heaps[handle].in_use)
        return (s32)CELL_SHEAP_ERROR_INVAL;
    u32 ptr_ea = (u32)(uintptr_t)ptr;
    if (!ptr_ea) return (s32)CELL_SHEAP_ERROR_INVAL;

    SheapState* h = &s_heaps[handle];
    for (int i = 0; i < h->blockCount; i++) {
        if (h->blocks[i].ea == ptr_ea) {
            /* Remove by shifting */
            for (int j = i; j < h->blockCount - 1; j++)
                h->blocks[j] = h->blocks[j + 1];
            h->blockCount--;
            return CELL_OK;
        }
    }
    return (s32)CELL_SHEAP_ERROR_INVAL;
}

s32 cellSheapQueryMax(CellSheapHandle handle, u32* maxFree)
{
    if (handle >= CELL_SHEAP_MAX || !s_heaps[handle].in_use)
        return (s32)CELL_SHEAP_ERROR_INVAL;
    if (!maxFree) return (s32)CELL_SHEAP_ERROR_INVAL;

    SheapState* h = &s_heaps[handle];
    u32 aligned_offset = align_up(h->offset, h->align);
    vm_write32((u32)(uintptr_t)maxFree, (aligned_offset < h->totalSize) ? h->totalSize - aligned_offset : 0);
    return CELL_OK;
}

s32 cellSheapQueryFree(CellSheapHandle handle, u32* freeBytes)
{
    /* Same as QueryMax for bump allocator */
    return cellSheapQueryMax(handle, freeBytes);
}

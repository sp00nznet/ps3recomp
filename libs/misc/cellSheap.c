/*
 * ps3recomp - cellSheap HLE implementation
 *
 * Simple bump allocator backed by a user-supplied buffer.
 * Uses a block tracking table for free/query support.
 */

#include "cellSheap.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

typedef struct {
    void* ptr;
    u32 size;
} SheapBlock;

typedef struct {
    int in_use;
    u8* base;
    u32 totalSize;
    u32 align;
    u32 offset;     /* bump pointer offset from base */
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

    if (!attr || !handle || !attr->heapBase || attr->heapSize == 0)
        return (s32)CELL_SHEAP_ERROR_INVAL;

    u32 alignment = attr->align;
    if (alignment == 0) alignment = 16;
    if (alignment & (alignment - 1))
        return (s32)CELL_SHEAP_ERROR_ALIGN;

    for (int i = 0; i < CELL_SHEAP_MAX; i++) {
        if (!s_heaps[i].in_use) {
            memset(&s_heaps[i], 0, sizeof(SheapState));
            s_heaps[i].in_use = 1;
            s_heaps[i].base = (u8*)attr->heapBase;
            s_heaps[i].totalSize = attr->heapSize;
            s_heaps[i].align = alignment;
            s_heaps[i].offset = 0;
            *handle = (u32)i;
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

    void* ptr = h->base + aligned_offset;
    h->blocks[h->blockCount].ptr = ptr;
    h->blocks[h->blockCount].size = aligned_size;
    h->blockCount++;
    h->offset = aligned_offset + aligned_size;

    return ptr;
}

s32 cellSheapFree(CellSheapHandle handle, void* ptr)
{
    if (handle >= CELL_SHEAP_MAX || !s_heaps[handle].in_use)
        return (s32)CELL_SHEAP_ERROR_INVAL;
    if (!ptr) return (s32)CELL_SHEAP_ERROR_INVAL;

    SheapState* h = &s_heaps[handle];
    for (int i = 0; i < h->blockCount; i++) {
        if (h->blocks[i].ptr == ptr) {
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
    *maxFree = (aligned_offset < h->totalSize) ? h->totalSize - aligned_offset : 0;
    return CELL_OK;
}

s32 cellSheapQueryFree(CellSheapHandle handle, u32* freeBytes)
{
    /* Same as QueryMax for bump allocator */
    return cellSheapQueryMax(handle, freeBytes);
}

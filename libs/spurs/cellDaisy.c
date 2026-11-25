/*
 * ps3recomp - cellDaisy HLE implementation
 *
 * SPURS Daisy Chain — lock-free FIFO queues.
 * Stub — pipes accept data and deliver it immediately (no SPU involvement).
 * Uses a simple ring buffer for each pipe.
 */

#include "cellDaisy.h"
#include "ps3emu/endian.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host pointer */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Internal state */

typedef struct {
    int  in_use;
    u32  direction;
    u32  entrySize;
    u32  depth;
    u32  head;     /* read index */
    u32  tail;     /* write index */
    u32  count;
    u8*  ring;     /* ring buffer */
} DaisyPipeSlot;

static DaisyPipeSlot s_pipes[CELL_DAISY_MAX_PIPES];

static int daisy_alloc(void)
{
    for (int i = 0; i < CELL_DAISY_MAX_PIPES; i++) {
        if (!s_pipes[i].in_use) return i;
    }
    return -1;
}

/* API */

s32 cellDaisyPipeAttributeInitialize(CellDaisyPipeAttribute* attr,
                                     u32 direction, u32 entrySize, u32 depth)
{
    if (!attr) return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;
    if (entrySize == 0 || depth == 0 || depth > CELL_DAISY_MAX_DEPTH)
        return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    attr = GUEST_PTR(attr, CellDaisyPipeAttribute*);
    memset(attr, 0, sizeof(*attr));
    attr->direction = ps3_bswap32(direction);
    attr->entrySize = ps3_bswap32(entrySize);
    attr->depth     = ps3_bswap32(depth);
    return CELL_OK;
}

s32 cellDaisyCreatePipe(void* spurs, CellDaisyPipe* pipe,
                        const CellDaisyPipeAttribute* attr,
                        void* buffer, u32 bufferSize)
{
    u32 pipe_ea = (u32)(uintptr_t)pipe;
    attr = GUEST_PTR(attr, const CellDaisyPipeAttribute*);
    (void)spurs; (void)buffer; (void)bufferSize;

    if (!pipe_ea || !attr) return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    u32 direction = ps3_bswap32(attr->direction);
    u32 entrySize = ps3_bswap32(attr->entrySize);
    u32 depth     = ps3_bswap32(attr->depth);
    printf("[cellDaisy] CreatePipe(entrySize=%u, depth=%u)\n", entrySize, depth);

    if (entrySize == 0 || depth == 0 || depth > CELL_DAISY_MAX_DEPTH)
        return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    int slot = daisy_alloc();
    if (slot < 0) return (s32)CELL_DAISY_ERROR_OUT_OF_MEMORY;

    DaisyPipeSlot* p = &s_pipes[slot];
    p->in_use = 1;
    p->direction = direction;
    p->entrySize = entrySize;
    p->depth = depth;
    p->head = 0;
    p->tail = 0;
    p->count = 0;

    /* Allocate ring buffer */
    p->ring = (u8*)malloc((size_t)entrySize * depth);
    if (!p->ring) {
        p->in_use = 0;
        return (s32)CELL_DAISY_ERROR_OUT_OF_MEMORY;
    }
    memset(p->ring, 0, (size_t)entrySize * depth);

    vm_write32(pipe_ea, (u32)slot);
    return CELL_OK;
}

s32 cellDaisyDestroyPipe(CellDaisyPipe* pipe)
{
    u32 pipe_ea = (u32)(uintptr_t)pipe;
    if (!pipe_ea) return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    u32 slot = vm_read32(pipe_ea);
    if (slot >= CELL_DAISY_MAX_PIPES || !s_pipes[slot].in_use)
        return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    printf("[cellDaisy] DestroyPipe(%u)\n", slot);
    free(s_pipes[slot].ring);
    memset(&s_pipes[slot], 0, sizeof(DaisyPipeSlot));
    return CELL_OK;
}

s32 cellDaisyPipePush(CellDaisyPipe* pipe, const void* data)
{
    u32 pipe_ea = (u32)(uintptr_t)pipe;
    data = GUEST_PTR(data, const void*);
    if (!pipe_ea || !data) return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    u32 slot = vm_read32(pipe_ea);
    if (slot >= CELL_DAISY_MAX_PIPES || !s_pipes[slot].in_use)
        return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    DaisyPipeSlot* p = &s_pipes[slot];
    if (p->count >= p->depth)
        return (s32)CELL_DAISY_ERROR_QUEUE_FULL;

    memcpy(p->ring + (p->tail * p->entrySize), data, p->entrySize);
    p->tail = (p->tail + 1) % p->depth;
    p->count++;
    return CELL_OK;
}

s32 cellDaisyPipeTryPush(CellDaisyPipe* pipe, const void* data)
{
    return cellDaisyPipePush(pipe, data);
}

s32 cellDaisyPipePop(CellDaisyPipe* pipe, void* data)
{
    u32 pipe_ea = (u32)(uintptr_t)pipe;
    data = GUEST_PTR(data, void*);
    if (!pipe_ea || !data) return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    u32 slot = vm_read32(pipe_ea);
    if (slot >= CELL_DAISY_MAX_PIPES || !s_pipes[slot].in_use)
        return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    DaisyPipeSlot* p = &s_pipes[slot];
    if (p->count == 0)
        return (s32)CELL_DAISY_ERROR_QUEUE_EMPTY;

    memcpy(data, p->ring + (p->head * p->entrySize), p->entrySize);
    p->head = (p->head + 1) % p->depth;
    p->count--;
    return CELL_OK;
}

s32 cellDaisyPipeTryPop(CellDaisyPipe* pipe, void* data)
{
    return cellDaisyPipePop(pipe, data);
}

s32 cellDaisyPipeGetCount(CellDaisyPipe* pipe, u32* count)
{
    u32 pipe_ea = (u32)(uintptr_t)pipe;
    if (!pipe_ea || !count) return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    u32 slot = vm_read32(pipe_ea);
    if (slot >= CELL_DAISY_MAX_PIPES || !s_pipes[slot].in_use)
        return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)count, s_pipes[slot].count);
    return CELL_OK;
}

s32 cellDaisyPipeGetFreeCount(CellDaisyPipe* pipe, u32* count)
{
    u32 pipe_ea = (u32)(uintptr_t)pipe;
    if (!pipe_ea || !count) return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    u32 slot = vm_read32(pipe_ea);
    if (slot >= CELL_DAISY_MAX_PIPES || !s_pipes[slot].in_use)
        return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)count, s_pipes[slot].depth - s_pipes[slot].count);
    return CELL_OK;
}

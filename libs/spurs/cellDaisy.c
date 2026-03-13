/*
 * ps3recomp - cellDaisy HLE implementation
 *
 * SPURS Daisy Chain — lock-free FIFO queues.
 * Stub — pipes accept data and deliver it immediately (no SPU involvement).
 * Uses a simple ring buffer for each pipe.
 */

#include "cellDaisy.h"
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

    memset(attr, 0, sizeof(*attr));
    attr->direction = direction;
    attr->entrySize = entrySize;
    attr->depth = depth;
    return CELL_OK;
}

s32 cellDaisyCreatePipe(void* spurs, CellDaisyPipe* pipe,
                        const CellDaisyPipeAttribute* attr,
                        void* buffer, u32 bufferSize)
{
    (void)spurs; (void)buffer; (void)bufferSize;
    printf("[cellDaisy] CreatePipe(entrySize=%u, depth=%u)\n",
           attr ? attr->entrySize : 0, attr ? attr->depth : 0);

    if (!pipe || !attr) return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    int slot = daisy_alloc();
    if (slot < 0) return (s32)CELL_DAISY_ERROR_OUT_OF_MEMORY;

    DaisyPipeSlot* p = &s_pipes[slot];
    p->in_use = 1;
    p->direction = attr->direction;
    p->entrySize = attr->entrySize;
    p->depth = attr->depth;
    p->head = 0;
    p->tail = 0;
    p->count = 0;

    /* Allocate ring buffer */
    p->ring = (u8*)malloc(attr->entrySize * attr->depth);
    if (!p->ring) {
        p->in_use = 0;
        return (s32)CELL_DAISY_ERROR_OUT_OF_MEMORY;
    }
    memset(p->ring, 0, attr->entrySize * attr->depth);

    *pipe = (CellDaisyPipe)slot;
    return CELL_OK;
}

s32 cellDaisyDestroyPipe(CellDaisyPipe* pipe)
{
    if (!pipe) return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    u32 slot = *pipe;
    if (slot >= CELL_DAISY_MAX_PIPES || !s_pipes[slot].in_use)
        return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    printf("[cellDaisy] DestroyPipe(%u)\n", slot);
    free(s_pipes[slot].ring);
    memset(&s_pipes[slot], 0, sizeof(DaisyPipeSlot));
    return CELL_OK;
}

s32 cellDaisyPipePush(CellDaisyPipe* pipe, const void* data)
{
    if (!pipe || !data) return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    u32 slot = *pipe;
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
    if (!pipe || !data) return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    u32 slot = *pipe;
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
    if (!pipe || !count) return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    u32 slot = *pipe;
    if (slot >= CELL_DAISY_MAX_PIPES || !s_pipes[slot].in_use)
        return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    *count = s_pipes[slot].count;
    return CELL_OK;
}

s32 cellDaisyPipeGetFreeCount(CellDaisyPipe* pipe, u32* count)
{
    if (!pipe || !count) return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    u32 slot = *pipe;
    if (slot >= CELL_DAISY_MAX_PIPES || !s_pipes[slot].in_use)
        return (s32)CELL_DAISY_ERROR_INVALID_ARGUMENT;

    *count = s_pipes[slot].depth - s_pipes[slot].count;
    return CELL_OK;
}

/*
 * ps3recomp - cellDaisy HLE
 *
 * SPURS Daisy Chain — lock-free FIFO queues for producer-consumer patterns
 * between PPU and SPU. Used for streaming data pipelines.
 * Stub — queue management works, data passes through immediately.
 */

#ifndef PS3RECOMP_CELL_DAISY_H
#define PS3RECOMP_CELL_DAISY_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_DAISY_ERROR_NOT_INITIALIZED     0x80410B01
#define CELL_DAISY_ERROR_ALREADY_INITIALIZED 0x80410B02
#define CELL_DAISY_ERROR_INVALID_ARGUMENT    0x80410B03
#define CELL_DAISY_ERROR_OUT_OF_MEMORY       0x80410B04
#define CELL_DAISY_ERROR_QUEUE_FULL          0x80410B05
#define CELL_DAISY_ERROR_QUEUE_EMPTY         0x80410B06
#define CELL_DAISY_ERROR_BUSY                0x80410B07

/* Pipe depth limits */
#define CELL_DAISY_MAX_PIPES     32
#define CELL_DAISY_MAX_DEPTH     256

/* Pipe direction */
#define CELL_DAISY_DIRECTION_PPU_TO_SPU   0
#define CELL_DAISY_DIRECTION_SPU_TO_PPU   1
#define CELL_DAISY_DIRECTION_SPU_TO_SPU   2

/* Opaque handles */
typedef u32 CellDaisyPipe;

/* Pipe attributes */
typedef struct CellDaisyPipeAttribute {
    u32 direction;
    u32 entrySize;
    u32 depth;
    u32 flags;
    u32 reserved[4];
} CellDaisyPipeAttribute;

/* Functions */
s32 cellDaisyPipeAttributeInitialize(CellDaisyPipeAttribute* attr,
                                     u32 direction, u32 entrySize, u32 depth);

s32 cellDaisyCreatePipe(void* spurs, CellDaisyPipe* pipe,
                        const CellDaisyPipeAttribute* attr,
                        void* buffer, u32 bufferSize);
s32 cellDaisyDestroyPipe(CellDaisyPipe* pipe);

s32 cellDaisyPipePush(CellDaisyPipe* pipe, const void* data);
s32 cellDaisyPipeTryPush(CellDaisyPipe* pipe, const void* data);
s32 cellDaisyPipePop(CellDaisyPipe* pipe, void* data);
s32 cellDaisyPipeTryPop(CellDaisyPipe* pipe, void* data);

s32 cellDaisyPipeGetCount(CellDaisyPipe* pipe, u32* count);
s32 cellDaisyPipeGetFreeCount(CellDaisyPipe* pipe, u32* count);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_DAISY_H */

/*
 * ps3recomp - cellSheap HLE
 *
 * Shared heap allocator for PPU/SPU shared memory regions.
 * Manages allocation within a user-provided buffer.
 */

#ifndef PS3RECOMP_CELL_SHEAP_H
#define PS3RECOMP_CELL_SHEAP_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_SHEAP_ERROR_INVAL       0x80410401
#define CELL_SHEAP_ERROR_NOMEM       0x80410402
#define CELL_SHEAP_ERROR_ALIGN       0x80410403
#define CELL_SHEAP_ERROR_NOSYS       0x80410404

/* Constants */
#define CELL_SHEAP_MAX   8
#define CELL_SHEAP_BLOCK_MAX  256

/* Types */
typedef u32 CellSheapHandle;

typedef struct CellSheapAttr {
    void* heapBase;
    u32 heapSize;
    u32 align;         /* minimum alignment, power of 2, >= 16 */
    u32 reserved[4];
} CellSheapAttr;

/* Functions */
s32 cellSheapInitialize(const CellSheapAttr* attr, CellSheapHandle* handle);
s32 cellSheapFinalize(CellSheapHandle handle);

void* cellSheapAllocate(CellSheapHandle handle, u32 size);
s32 cellSheapFree(CellSheapHandle handle, void* ptr);

s32 cellSheapQueryMax(CellSheapHandle handle, u32* maxFree);
s32 cellSheapQueryFree(CellSheapHandle handle, u32* freeBytes);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SHEAP_H */

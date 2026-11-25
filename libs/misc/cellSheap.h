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

/* Error codes (PS3 cellSheap, base 0x80410300; verified against Sony LLE libsre.sprx on RPCS3) */
#define CELL_SHEAP_ERROR_INVAL        (CELL_ERROR_BASE_SHEAP | 0x02) /* 0x80410302 */
#define CELL_SHEAP_ERROR_NOSYS        (CELL_ERROR_BASE_SHEAP | 0x03) /* 0x80410303 */
#define CELL_SHEAP_ERROR_NOMEM        (CELL_ERROR_BASE_SHEAP | 0x04) /* 0x80410304 */
#define CELL_SHEAP_ERROR_ALIGN        (CELL_ERROR_BASE_SHEAP | 0x10) /* 0x80410310 */
#define CELL_SHEAP_ERROR_NULL_POINTER (CELL_ERROR_BASE_SHEAP | 0x11) /* 0x80410311 */

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

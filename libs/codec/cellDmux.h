/*
 * ps3recomp - cellDmux HLE
 *
 * AV stream demultiplexer. Splits multiplexed PAMF streams into
 * individual elementary streams (ES) for feeding to cellVdec/cellAdec.
 */

#ifndef PS3RECOMP_CELL_DMUX_H
#define PS3RECOMP_CELL_DMUX_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_DMUX_ERROR_ARG            0x80610201
#define CELL_DMUX_ERROR_SEQ            0x80610202
#define CELL_DMUX_ERROR_BUSY           0x80610203
#define CELL_DMUX_ERROR_EMPTY          0x80610204
#define CELL_DMUX_ERROR_FATAL          0x80610205

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define CELL_DMUX_MAX_HANDLES          8
#define CELL_DMUX_MAX_ES               16

/* Demux types */
#define CELL_DMUX_TYPE_PAMF            0

/* ES message types (delivered via callback) */
#define CELL_DMUX_MSG_TYPE_DEMUX_DONE      0
#define CELL_DMUX_MSG_TYPE_FATAL_ERR       1
#define CELL_DMUX_MSG_TYPE_PROG_END_CODE   2

#define CELL_DMUX_ES_MSG_TYPE_AU_FOUND     0
#define CELL_DMUX_ES_MSG_TYPE_FLUSH_DONE   1

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

typedef u32 CellDmuxHandle;
typedef u32 CellDmuxEsHandle;

typedef struct CellDmuxType {
    u32 streamType;
    u32 reserved[3];
} CellDmuxType;

typedef struct CellDmuxResource {
    u32 memAddr;
    u32 memSize;
    u32 ppuThreadPrio;
    u32 ppuThreadStackSize;
    u32 spuThreadPrio;
    u32 numOfSpus;
} CellDmuxResource;

typedef struct CellDmuxEsFilterId {
    u8 filterIdMajor;
    u8 filterIdMinor;
    u8 supplementalInfo1;
    u8 supplementalInfo2;
} CellDmuxEsFilterId;

typedef struct CellDmuxEsResource {
    u32 memAddr;
    u32 memSize;
} CellDmuxEsResource;

typedef struct CellDmuxAuInfo {
    u32 auAddr;
    u32 auSize;
    u64 pts;
    u64 dts;
    u64 userData;
    u32 isRap;       /* random access point */
    u32 reserved;
} CellDmuxAuInfo;

typedef struct CellDmuxAuInfoEx {
    u32 auAddr;
    u32 auSize;
    u64 pts;
    u64 dts;
    u64 userData;
    u32 isRap;
    u32 reserved;
    u32 auSpecificInfo;
} CellDmuxAuInfoEx;

/* Callback types */
typedef u32 (*CellDmuxCbMsg)(u32 handle, u32 msgType, u32 msgData, void* cbArg);
typedef u32 (*CellDmuxCbEsMsg)(u32 handle, u32 esHandle, u32 msgType,
                                 u32 msgData, void* cbArg);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* Demuxer lifecycle */
s32 cellDmuxOpen(const CellDmuxType* type, const CellDmuxResource* res,
                  CellDmuxCbMsg cbFunc, void* cbArg, CellDmuxHandle* handle);
s32 cellDmuxClose(CellDmuxHandle handle);

/* ES management */
s32 cellDmuxEnableEs(CellDmuxHandle handle, const CellDmuxEsFilterId* esFilterId,
                      const CellDmuxEsResource* esRes,
                      CellDmuxCbEsMsg esCbFunc, void* esCbArg,
                      CellDmuxEsHandle* esHandle);
s32 cellDmuxDisableEs(CellDmuxEsHandle esHandle);

/* Data feeding */
s32 cellDmuxSetStream(CellDmuxHandle handle, u32 streamAddr, u32 streamSize,
                       b8 discontinuity, u64 userData);
s32 cellDmuxResetStream(CellDmuxHandle handle);
s32 cellDmuxResetStreamAndWaitDone(CellDmuxHandle handle);

/* AU retrieval */
s32 cellDmuxGetAu(CellDmuxEsHandle esHandle, CellDmuxAuInfo** auInfo, u32* auInfoNum);
s32 cellDmuxPeekAu(CellDmuxEsHandle esHandle, CellDmuxAuInfo** auInfo, u32* auInfoNum);
s32 cellDmuxReleaseAu(CellDmuxEsHandle esHandle);
s32 cellDmuxFlushEs(CellDmuxEsHandle esHandle);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_DMUX_H */

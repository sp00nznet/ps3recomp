/*
 * ps3recomp - cellVdecDivx HLE
 *
 * DivX video decoder for PS3. Used for DivX/Xvid media playback.
 * Stub — decoder management works, decode is a no-op.
 */

#ifndef PS3RECOMP_CELL_VDEC_DIVX_H
#define PS3RECOMP_CELL_VDEC_DIVX_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_VDEC_DIVX_ERROR_NOT_INITIALIZED     0x80610D01
#define CELL_VDEC_DIVX_ERROR_ALREADY_INITIALIZED 0x80610D02
#define CELL_VDEC_DIVX_ERROR_INVALID_ARGUMENT    0x80610D03
#define CELL_VDEC_DIVX_ERROR_OUT_OF_MEMORY       0x80610D04
#define CELL_VDEC_DIVX_ERROR_DECODE_FAILED       0x80610D05
#define CELL_VDEC_DIVX_ERROR_NOT_SUPPORTED       0x80610D06

/* Profiles */
#define CELL_VDEC_DIVX_PROFILE_MOBILE     0
#define CELL_VDEC_DIVX_PROFILE_HOME       1
#define CELL_VDEC_DIVX_PROFILE_HD         2

/* Handle */
typedef u32 CellVdecDivxHandle;

/* Config */
typedef struct CellVdecDivxConfig {
    u32 profile;
    u32 maxWidth;
    u32 maxHeight;
    u32 flags;
    u32 reserved[4];
} CellVdecDivxConfig;

/* Frame info */
typedef struct CellVdecDivxFrameInfo {
    u32 width;
    u32 height;
    u32 cropLeft;
    u32 cropTop;
    u32 cropRight;
    u32 cropBottom;
    u64 pts;
} CellVdecDivxFrameInfo;

/* Functions */
s32 cellVdecDivxOpen(const CellVdecDivxConfig* config, CellVdecDivxHandle* handle);
s32 cellVdecDivxClose(CellVdecDivxHandle handle);
s32 cellVdecDivxDecode(CellVdecDivxHandle handle, const void* au, u32 auSize,
                        void* picOut, u32 picBufSize, CellVdecDivxFrameInfo* info);
s32 cellVdecDivxReset(CellVdecDivxHandle handle);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_VDEC_DIVX_H */

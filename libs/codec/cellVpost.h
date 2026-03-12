/*
 * ps3recomp - cellVpost HLE
 *
 * Video post-processing: color conversion (YUV->RGB), scaling,
 * deinterlacing, rotation. Stub — executes callbacks without actual processing.
 */

#ifndef PS3RECOMP_CELL_VPOST_H
#define PS3RECOMP_CELL_VPOST_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_VPOST_ERROR_NOT_INITIALIZED     0x80610001
#define CELL_VPOST_ERROR_ALREADY_INITIALIZED 0x80610002
#define CELL_VPOST_ERROR_INVALID_ARGUMENT    0x80610003
#define CELL_VPOST_ERROR_OUT_OF_MEMORY       0x80610004
#define CELL_VPOST_ERROR_HANDLE_NOT_FOUND    0x80610005

/* Constants */
#define CELL_VPOST_HANDLE_MAX    4

/* Picture format */
#define CELL_VPOST_PIC_FMT_YUV420P    0
#define CELL_VPOST_PIC_FMT_YUV422P    1
#define CELL_VPOST_PIC_FMT_RGBA8888   2
#define CELL_VPOST_PIC_FMT_ARGB8888   3

/* Scan type */
#define CELL_VPOST_SCAN_PROGRESSIVE  0
#define CELL_VPOST_SCAN_INTERLACE    1

/* Types */
typedef u32 CellVpostHandle;

typedef struct CellVpostCfgParam {
    u32 inMaxWidth;
    u32 inMaxHeight;
    u32 inPicFmt;
    u32 outMaxWidth;
    u32 outMaxHeight;
    u32 outPicFmt;
    u32 execThread;
    u32 reserved;
} CellVpostCfgParam;

typedef struct CellVpostResource {
    void* memAddr;
    u32 memSize;
    s32 ppuThreadPriority;
    u32 ppuThreadStackSize;
} CellVpostResource;

typedef struct CellVpostPictureInfo {
    u32 width;
    u32 height;
    u32 picFmt;
    u32 scanType;
    u32 codecType;
    u32 reserved[3];
} CellVpostPictureInfo;

typedef struct CellVpostCtrlParam {
    u32 outWidth;
    u32 outHeight;
    u32 outPicFmt;
    u32 execType;
    u32 reserved[4];
} CellVpostCtrlParam;

typedef void (*CellVpostExecCb)(u32 handle, s32 result, void* arg);

/* Functions */
s32 cellVpostInit(const CellVpostCfgParam* cfgParam,
                    const CellVpostResource* resource,
                    CellVpostHandle* handle);
s32 cellVpostEnd(CellVpostHandle handle);

s32 cellVpostExec(CellVpostHandle handle,
                    const void* inPicBuf,
                    const CellVpostPictureInfo* picInfo,
                    void* outPicBuf,
                    const CellVpostCtrlParam* ctrlParam);

s32 cellVpostQuery(const CellVpostCfgParam* cfgParam, u32* memSize);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_VPOST_H */

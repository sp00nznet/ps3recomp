/*
 * ps3recomp - cellCamera HLE
 *
 * PlayStation Eye camera access. Stub — reports no camera attached.
 * Games handle gracefully with "camera not found" UI.
 */

#ifndef PS3RECOMP_CELL_CAMERA_H
#define PS3RECOMP_CELL_CAMERA_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_CAMERA_ERROR_NOT_INITIALIZED     0x80140801
#define CELL_CAMERA_ERROR_ALREADY_INITIALIZED 0x80140802
#define CELL_CAMERA_ERROR_INVALID_ARGUMENT    0x80140803
#define CELL_CAMERA_ERROR_NOT_OPEN            0x80140804
#define CELL_CAMERA_ERROR_DEVICE_NOT_FOUND    0x80140805
#define CELL_CAMERA_ERROR_DEVICE_BUSY         0x80140806
#define CELL_CAMERA_ERROR_NOT_STARTED         0x80140807

/* Camera types */
#define CELL_CAMERA_TYPE_UNKNOWN        0
#define CELL_CAMERA_TYPE_EYETOY         1
#define CELL_CAMERA_TYPE_EYETOY2        2
#define CELL_CAMERA_TYPE_USBVIDEO       3

/* Resolution */
#define CELL_CAMERA_VGA   0  /* 640x480 */
#define CELL_CAMERA_QVGA  1  /* 320x240 */

/* Format */
#define CELL_CAMERA_FORMAT_RAW8    0
#define CELL_CAMERA_FORMAT_YUV422  1
#define CELL_CAMERA_FORMAT_YUV420  2
#define CELL_CAMERA_FORMAT_RGBA    3

/* Types */
typedef struct CellCameraInfo {
    u32 format;
    u32 resolution;
    u32 framerate;
    void* buffer;
    u32 bufferSize;
    u32 width;
    u32 height;
    u32 reserved[4];
} CellCameraInfo;

typedef struct CellCameraReadInfo {
    u32 status;
    u32 readCount;
    u64 timestamp;
    void* buffer;
    u32 bufferSize;
} CellCameraReadInfo;

typedef void (*CellCameraCallback)(s32 result, void* arg);

/* Functions */
s32 cellCameraInit(void);
s32 cellCameraEnd(void);

s32 cellCameraOpen(s32 devNum, CellCameraInfo* info);
s32 cellCameraClose(s32 devNum);

s32 cellCameraStart(s32 devNum);
s32 cellCameraStop(s32 devNum);

s32 cellCameraRead(s32 devNum, CellCameraReadInfo* info);
s32 cellCameraReadEx(s32 devNum, CellCameraReadInfo* info);

s32 cellCameraIsAvailable(s32 devNum);
s32 cellCameraIsAttached(s32 devNum);
s32 cellCameraIsOpen(s32 devNum);
s32 cellCameraIsStarted(s32 devNum);

s32 cellCameraGetType(s32 devNum, s32* type);

s32 cellCameraSetNotifyCallback(s32 devNum, CellCameraCallback cb, void* arg);
s32 cellCameraRemoveNotifyCallback(s32 devNum);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_CAMERA_H */

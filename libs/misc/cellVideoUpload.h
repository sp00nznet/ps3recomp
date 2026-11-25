/*
 * ps3recomp - cellVideoUpload HLE
 *
 * Video upload to social networking services (YouTube, etc.).
 * Stub — init/term work, upload returns NOT_SUPPORTED.
 */

#ifndef PS3RECOMP_CELL_VIDEO_UPLOAD_H
#define PS3RECOMP_CELL_VIDEO_UPLOAD_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (sysutil/sysutil_video_upload.h; base 0x8002D000 verified against
 * Sony firmware 4.93 libsysutil_video_upload.sprx: Initialize(0) -> 0x8002D022) */
#define CELL_VIDEO_UPLOAD_ERROR_NOT_INITIALIZED     (CELL_ERROR_BASE_SYSUTIL_VIDEO_UPLOAD | 0x01) /* 0x8002D001 */
#define CELL_VIDEO_UPLOAD_ERROR_ALREADY_INITIALIZED (CELL_ERROR_BASE_SYSUTIL_VIDEO_UPLOAD | 0x02) /* 0x8002D002 */
#define CELL_VIDEO_UPLOAD_ERROR_PARAM               (CELL_ERROR_BASE_SYSUTIL_VIDEO_UPLOAD | 0x22) /* 0x8002D022 */
#define CELL_VIDEO_UPLOAD_ERROR_INVALID_ARGUMENT    CELL_VIDEO_UPLOAD_ERROR_PARAM
#define CELL_VIDEO_UPLOAD_ERROR_NOT_SUPPORTED       (CELL_ERROR_BASE_SYSUTIL_VIDEO_UPLOAD | 0x04) /* 0x8002D004 */

/* Types */
typedef struct CellVideoUploadParam {
    const char* filePath;
    const char* title;
    const char* description;
    const char* tags;
    u32 reserved[4];
} CellVideoUploadParam;

typedef void (*CellVideoUploadCallback)(s32 result, void* arg);

/* Functions */
s32 cellVideoUploadInit(void);
s32 cellVideoUploadTerm(void);

s32 cellVideoUploadStart(const CellVideoUploadParam* param,
                           CellVideoUploadCallback callback, void* arg);
s32 cellVideoUploadCancel(void);
s32 cellVideoUploadGetStatus(s32* status);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_VIDEO_UPLOAD_H */

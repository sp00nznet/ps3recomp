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

/* Error codes */
#define CELL_VIDEO_UPLOAD_ERROR_NOT_INITIALIZED     0x8002C101
#define CELL_VIDEO_UPLOAD_ERROR_ALREADY_INITIALIZED 0x8002C102
#define CELL_VIDEO_UPLOAD_ERROR_INVALID_ARGUMENT    0x8002C103
#define CELL_VIDEO_UPLOAD_ERROR_NOT_SUPPORTED       0x8002C104

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

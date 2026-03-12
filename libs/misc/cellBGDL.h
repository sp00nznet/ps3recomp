/*
 * ps3recomp - cellBGDL HLE
 *
 * Background download manager for DLC/patches.
 * Stub — init/term work, no downloads occur.
 */

#ifndef PS3RECOMP_CELL_BGDL_H
#define PS3RECOMP_CELL_BGDL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_BGDL_ERROR_NOT_INITIALIZED     0x80410901
#define CELL_BGDL_ERROR_ALREADY_INITIALIZED 0x80410902
#define CELL_BGDL_ERROR_INVALID_ARGUMENT    0x80410903
#define CELL_BGDL_ERROR_NOT_FOUND           0x80410904
#define CELL_BGDL_ERROR_BUSY                0x80410905

/* Download status */
#define CELL_BGDL_STATUS_IDLE         0
#define CELL_BGDL_STATUS_DOWNLOADING  1
#define CELL_BGDL_STATUS_PAUSED       2
#define CELL_BGDL_STATUS_COMPLETE     3
#define CELL_BGDL_STATUS_ERROR        4

/* Types */
typedef u32 CellBgdlId;

typedef struct CellBgdlInfo {
    CellBgdlId id;
    s32 status;
    u64 totalSize;
    u64 downloadedSize;
    char contentId[48];
    u32 reserved[4];
} CellBgdlInfo;

/* Functions */
s32 cellBgdlInit(void);
s32 cellBgdlTerm(void);

s32 cellBgdlGetInfo(CellBgdlId id, CellBgdlInfo* info);
s32 cellBgdlGetList(CellBgdlInfo* list, u32 maxEntries, u32* numEntries);

s32 cellBgdlStartDownload(const char* url, const char* contentId,
                            CellBgdlId* id);
s32 cellBgdlPauseDownload(CellBgdlId id);
s32 cellBgdlResumeDownload(CellBgdlId id);
s32 cellBgdlCancelDownload(CellBgdlId id);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_BGDL_H */

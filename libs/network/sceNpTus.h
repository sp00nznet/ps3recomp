/*
 * ps3recomp - sceNpTus HLE
 *
 * NP Title User Storage: per-user cloud storage for game data
 * (leaderboards, stats, etc.). Offline stub that stores data locally.
 */

#ifndef PS3RECOMP_SCE_NP_TUS_H
#define PS3RECOMP_SCE_NP_TUS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define SCE_NP_TUS_ERROR_NOT_INITIALIZED       0x80022E01
#define SCE_NP_TUS_ERROR_ALREADY_INITIALIZED   0x80022E02
#define SCE_NP_TUS_ERROR_INVALID_ARGUMENT      0x80022E03
#define SCE_NP_TUS_ERROR_OUT_OF_MEMORY         0x80022E04
#define SCE_NP_TUS_ERROR_NOT_CONNECTED         0x80022E05
#define SCE_NP_TUS_ERROR_DATA_NOT_FOUND        0x80022E06
#define SCE_NP_TUS_ERROR_TIMEOUT               0x80022E07

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define SCE_NP_TUS_MAX_SLOT_NUM           64
#define SCE_NP_TUS_DATA_INFO_MAX_SIZE     384
#define SCE_NP_TUS_MAX_CTX               8

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef u32 SceNpTusCtx;
typedef u32 SceNpTusRequestId;

typedef struct SceNpTusVariable {
    s64 variable;
    u64 lastChangedDate;
    char ownerNpId[20];
    u8  hasData;
    u8  reserved[3];
} SceNpTusVariable;

typedef struct SceNpTusDataInfo {
    u32 infoSize;
    u8  data[SCE_NP_TUS_DATA_INFO_MAX_SIZE];
} SceNpTusDataInfo;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* Lifecycle */
s32 sceNpTusInit(s32 prio, s32 stackSize, void* option);
s32 sceNpTusTerm(void);

/* Context management */
s32 sceNpTusCreateCtx(SceNpTusCtx* ctx);
s32 sceNpTusDestroyCtx(SceNpTusCtx ctx);

/* Variable operations (leaderboard scores, etc.) */
s32 sceNpTusSetVariable(SceNpTusCtx ctx, u32 slotId, s64 variable,
                          void* option);
s32 sceNpTusGetVariable(SceNpTusCtx ctx, const void* npId, u32 slotId,
                          SceNpTusVariable* outVariable, void* option);
s32 sceNpTusTryAndSetVariable(SceNpTusCtx ctx, u32 slotId,
                                s32 opType, s64 variable,
                                SceNpTusVariable* outVariable, void* option);
s32 sceNpTusAddAndGetVariable(SceNpTusCtx ctx, u32 slotId,
                                s64 inVariable, SceNpTusVariable* outVariable,
                                void* option);
s32 sceNpTusDeleteMultiSlotVariable(SceNpTusCtx ctx, const u32* slotIds,
                                      u32 slotNum, void* option);

/* Data operations (binary blobs) */
s32 sceNpTusSetData(SceNpTusCtx ctx, u32 slotId,
                      const void* data, u32 dataSize,
                      const SceNpTusDataInfo* info, void* option);
s32 sceNpTusGetData(SceNpTusCtx ctx, const void* npId, u32 slotId,
                      void* data, u32 dataSize, u32* outDataSize,
                      SceNpTusDataInfo* outInfo, void* option);
s32 sceNpTusDeleteMultiSlotData(SceNpTusCtx ctx, const u32* slotIds,
                                  u32 slotNum, void* option);

/* Poll for async completion */
s32 sceNpTusPollAsync(SceNpTusRequestId reqId, s32* result);
s32 sceNpTusWaitAsync(SceNpTusRequestId reqId, s32* result);
s32 sceNpTusAbortRequest(SceNpTusRequestId reqId);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_TUS_H */

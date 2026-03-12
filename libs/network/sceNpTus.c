/*
 * ps3recomp - sceNpTus HLE implementation
 *
 * Local storage for TUS variables and data. Games can set/get
 * leaderboard scores and binary blobs, stored in memory only
 * (no PSN connectivity required).
 */

#include "sceNpTus.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_initialized = 0;

typedef struct {
    int in_use;
} TusCtxSlot;

static TusCtxSlot s_ctx[SCE_NP_TUS_MAX_CTX];

/* Local variable storage */
typedef struct {
    int valid;
    s64 value;
    u64 timestamp;
} TusVarSlot;

static TusVarSlot s_variables[SCE_NP_TUS_MAX_SLOT_NUM];

/* Local data storage */
typedef struct {
    int valid;
    u8* data;
    u32 dataSize;
    SceNpTusDataInfo info;
} TusDataSlot;

static TusDataSlot s_data[SCE_NP_TUS_MAX_SLOT_NUM];

/* ---------------------------------------------------------------------------
 * Lifecycle
 * -----------------------------------------------------------------------*/

s32 sceNpTusInit(s32 prio, s32 stackSize, void* option)
{
    (void)prio; (void)stackSize; (void)option;
    printf("[sceNpTus] Init()\n");

    if (s_initialized)
        return (s32)SCE_NP_TUS_ERROR_ALREADY_INITIALIZED;

    memset(s_ctx, 0, sizeof(s_ctx));
    memset(s_variables, 0, sizeof(s_variables));
    memset(s_data, 0, sizeof(s_data));
    s_initialized = 1;
    return CELL_OK;
}

s32 sceNpTusTerm(void)
{
    printf("[sceNpTus] Term()\n");

    if (!s_initialized)
        return (s32)SCE_NP_TUS_ERROR_NOT_INITIALIZED;

    /* Free allocated data blobs */
    for (int i = 0; i < SCE_NP_TUS_MAX_SLOT_NUM; i++) {
        if (s_data[i].valid && s_data[i].data) {
            free(s_data[i].data);
            s_data[i].data = NULL;
        }
    }

    s_initialized = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Context management
 * -----------------------------------------------------------------------*/

s32 sceNpTusCreateCtx(SceNpTusCtx* ctx)
{
    printf("[sceNpTus] CreateCtx()\n");

    if (!s_initialized)
        return (s32)SCE_NP_TUS_ERROR_NOT_INITIALIZED;
    if (!ctx)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < SCE_NP_TUS_MAX_CTX; i++) {
        if (!s_ctx[i].in_use) {
            s_ctx[i].in_use = 1;
            *ctx = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)SCE_NP_TUS_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTusDestroyCtx(SceNpTusCtx ctx)
{
    printf("[sceNpTus] DestroyCtx(ctx=%u)\n", ctx);

    if (ctx >= SCE_NP_TUS_MAX_CTX || !s_ctx[ctx].in_use)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    s_ctx[ctx].in_use = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Variable operations
 * -----------------------------------------------------------------------*/

s32 sceNpTusSetVariable(SceNpTusCtx ctx, u32 slotId, s64 variable,
                          void* option)
{
    (void)ctx; (void)option;

    printf("[sceNpTus] SetVariable(slot=%u, val=%lld)\n",
           slotId, (long long)variable);

    if (!s_initialized)
        return (s32)SCE_NP_TUS_ERROR_NOT_INITIALIZED;
    if (slotId >= SCE_NP_TUS_MAX_SLOT_NUM)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    s_variables[slotId].valid = 1;
    s_variables[slotId].value = variable;
    s_variables[slotId].timestamp++;
    return CELL_OK;
}

s32 sceNpTusGetVariable(SceNpTusCtx ctx, const void* npId, u32 slotId,
                          SceNpTusVariable* outVariable, void* option)
{
    (void)ctx; (void)npId; (void)option;

    if (!s_initialized)
        return (s32)SCE_NP_TUS_ERROR_NOT_INITIALIZED;
    if (slotId >= SCE_NP_TUS_MAX_SLOT_NUM || !outVariable)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    if (!s_variables[slotId].valid)
        return (s32)SCE_NP_TUS_ERROR_DATA_NOT_FOUND;

    memset(outVariable, 0, sizeof(*outVariable));
    outVariable->variable = s_variables[slotId].value;
    outVariable->lastChangedDate = s_variables[slotId].timestamp;
    outVariable->hasData = 1;
    strncpy(outVariable->ownerNpId, "ps3recomp_user", sizeof(outVariable->ownerNpId) - 1);
    return CELL_OK;
}

s32 sceNpTusTryAndSetVariable(SceNpTusCtx ctx, u32 slotId,
                                s32 opType, s64 variable,
                                SceNpTusVariable* outVariable, void* option)
{
    (void)opType;
    s32 rc = sceNpTusSetVariable(ctx, slotId, variable, option);
    if (rc == CELL_OK && outVariable) {
        sceNpTusGetVariable(ctx, NULL, slotId, outVariable, option);
    }
    return rc;
}

s32 sceNpTusAddAndGetVariable(SceNpTusCtx ctx, u32 slotId,
                                s64 inVariable, SceNpTusVariable* outVariable,
                                void* option)
{
    if (slotId >= SCE_NP_TUS_MAX_SLOT_NUM)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    s64 current = s_variables[slotId].valid ? s_variables[slotId].value : 0;
    return sceNpTusTryAndSetVariable(ctx, slotId, 0, current + inVariable,
                                      outVariable, option);
}

s32 sceNpTusDeleteMultiSlotVariable(SceNpTusCtx ctx, const u32* slotIds,
                                      u32 slotNum, void* option)
{
    (void)ctx; (void)option;

    if (!slotIds)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    for (u32 i = 0; i < slotNum; i++) {
        if (slotIds[i] < SCE_NP_TUS_MAX_SLOT_NUM)
            s_variables[slotIds[i]].valid = 0;
    }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Data operations
 * -----------------------------------------------------------------------*/

s32 sceNpTusSetData(SceNpTusCtx ctx, u32 slotId,
                      const void* data, u32 dataSize,
                      const SceNpTusDataInfo* info, void* option)
{
    (void)ctx; (void)option;

    printf("[sceNpTus] SetData(slot=%u, size=%u)\n", slotId, dataSize);

    if (!s_initialized)
        return (s32)SCE_NP_TUS_ERROR_NOT_INITIALIZED;
    if (slotId >= SCE_NP_TUS_MAX_SLOT_NUM || !data)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    /* Free existing data */
    if (s_data[slotId].data) {
        free(s_data[slotId].data);
        s_data[slotId].data = NULL;
    }

    s_data[slotId].data = (u8*)malloc(dataSize);
    if (!s_data[slotId].data)
        return (s32)SCE_NP_TUS_ERROR_OUT_OF_MEMORY;

    memcpy(s_data[slotId].data, data, dataSize);
    s_data[slotId].dataSize = dataSize;
    s_data[slotId].valid = 1;

    if (info)
        s_data[slotId].info = *info;
    else
        memset(&s_data[slotId].info, 0, sizeof(SceNpTusDataInfo));

    return CELL_OK;
}

s32 sceNpTusGetData(SceNpTusCtx ctx, const void* npId, u32 slotId,
                      void* data, u32 dataSize, u32* outDataSize,
                      SceNpTusDataInfo* outInfo, void* option)
{
    (void)ctx; (void)npId; (void)option;

    if (!s_initialized)
        return (s32)SCE_NP_TUS_ERROR_NOT_INITIALIZED;
    if (slotId >= SCE_NP_TUS_MAX_SLOT_NUM)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    if (!s_data[slotId].valid)
        return (s32)SCE_NP_TUS_ERROR_DATA_NOT_FOUND;

    u32 copySize = s_data[slotId].dataSize;
    if (copySize > dataSize)
        copySize = dataSize;

    if (data && copySize > 0)
        memcpy(data, s_data[slotId].data, copySize);

    if (outDataSize)
        *outDataSize = s_data[slotId].dataSize;

    if (outInfo)
        *outInfo = s_data[slotId].info;

    return CELL_OK;
}

s32 sceNpTusDeleteMultiSlotData(SceNpTusCtx ctx, const u32* slotIds,
                                  u32 slotNum, void* option)
{
    (void)ctx; (void)option;

    if (!slotIds)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    for (u32 i = 0; i < slotNum; i++) {
        if (slotIds[i] < SCE_NP_TUS_MAX_SLOT_NUM && s_data[slotIds[i]].valid) {
            free(s_data[slotIds[i]].data);
            s_data[slotIds[i]].data = NULL;
            s_data[slotIds[i]].valid = 0;
        }
    }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Async polling (all operations are synchronous in this stub)
 * -----------------------------------------------------------------------*/

s32 sceNpTusPollAsync(SceNpTusRequestId reqId, s32* result)
{
    (void)reqId;
    if (result) *result = 0; /* always done */
    return CELL_OK;
}

s32 sceNpTusWaitAsync(SceNpTusRequestId reqId, s32* result)
{
    (void)reqId;
    if (result) *result = 0;
    return CELL_OK;
}

s32 sceNpTusAbortRequest(SceNpTusRequestId reqId)
{
    (void)reqId;
    return CELL_OK;
}

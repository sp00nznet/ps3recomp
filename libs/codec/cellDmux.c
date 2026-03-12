/*
 * ps3recomp - cellDmux HLE implementation
 *
 * Stub demuxer that accepts PAMF data and reports AU-found callbacks.
 * Without real MPEG-TS parsing, this provides the API surface games
 * expect while the video pipeline is built out.
 */

#include "cellDmux.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    int in_use;
    CellDmuxCbMsg cbFunc;
    void* cbArg;
    u32 streamType;
} DmuxSlot;

typedef struct {
    int in_use;
    u32 dmuxId;
    CellDmuxEsFilterId filterId;
    CellDmuxCbEsMsg esCbFunc;
    void* esCbArg;
    CellDmuxAuInfo currentAu;
    int hasAu;
} DmuxEsSlot;

static DmuxSlot s_dmux[CELL_DMUX_MAX_HANDLES];
static DmuxEsSlot s_es[CELL_DMUX_MAX_ES];

/* ---------------------------------------------------------------------------
 * Demuxer lifecycle
 * -----------------------------------------------------------------------*/

s32 cellDmuxOpen(const CellDmuxType* type, const CellDmuxResource* res,
                  CellDmuxCbMsg cbFunc, void* cbArg, CellDmuxHandle* handle)
{
    (void)res;

    printf("[cellDmux] Open(streamType=%u)\n",
           type ? type->streamType : 0);

    if (!type || !handle)
        return (s32)CELL_DMUX_ERROR_ARG;

    for (int i = 0; i < CELL_DMUX_MAX_HANDLES; i++) {
        if (!s_dmux[i].in_use) {
            s_dmux[i].in_use = 1;
            s_dmux[i].cbFunc = cbFunc;
            s_dmux[i].cbArg = cbArg;
            s_dmux[i].streamType = type->streamType;
            *handle = (u32)i;
            printf("[cellDmux] Open -> handle=%u\n", i);
            return CELL_OK;
        }
    }
    return (s32)CELL_DMUX_ERROR_BUSY;
}

s32 cellDmuxClose(CellDmuxHandle handle)
{
    printf("[cellDmux] Close(handle=%u)\n", handle);

    if (handle >= CELL_DMUX_MAX_HANDLES || !s_dmux[handle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    /* Close associated ES handles */
    for (int i = 0; i < CELL_DMUX_MAX_ES; i++) {
        if (s_es[i].in_use && s_es[i].dmuxId == handle)
            s_es[i].in_use = 0;
    }

    s_dmux[handle].in_use = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * ES management
 * -----------------------------------------------------------------------*/

s32 cellDmuxEnableEs(CellDmuxHandle handle, const CellDmuxEsFilterId* esFilterId,
                      const CellDmuxEsResource* esRes,
                      CellDmuxCbEsMsg esCbFunc, void* esCbArg,
                      CellDmuxEsHandle* esHandle)
{
    (void)esRes;

    printf("[cellDmux] EnableEs(dmux=%u, major=%u, minor=%u)\n",
           handle,
           esFilterId ? esFilterId->filterIdMajor : 0,
           esFilterId ? esFilterId->filterIdMinor : 0);

    if (handle >= CELL_DMUX_MAX_HANDLES || !s_dmux[handle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;
    if (!esFilterId || !esHandle)
        return (s32)CELL_DMUX_ERROR_ARG;

    for (int i = 0; i < CELL_DMUX_MAX_ES; i++) {
        if (!s_es[i].in_use) {
            s_es[i].in_use = 1;
            s_es[i].dmuxId = handle;
            s_es[i].filterId = *esFilterId;
            s_es[i].esCbFunc = esCbFunc;
            s_es[i].esCbArg = esCbArg;
            s_es[i].hasAu = 0;
            memset(&s_es[i].currentAu, 0, sizeof(CellDmuxAuInfo));
            *esHandle = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)CELL_DMUX_ERROR_BUSY;
}

s32 cellDmuxDisableEs(CellDmuxEsHandle esHandle)
{
    printf("[cellDmux] DisableEs(es=%u)\n", esHandle);

    if (esHandle >= CELL_DMUX_MAX_ES || !s_es[esHandle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    s_es[esHandle].in_use = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Data feeding
 * -----------------------------------------------------------------------*/

s32 cellDmuxSetStream(CellDmuxHandle handle, u32 streamAddr, u32 streamSize,
                       b8 discontinuity, u64 userData)
{
    (void)discontinuity;
    (void)userData;

    printf("[cellDmux] SetStream(handle=%u, addr=0x%X, size=%u)\n",
           handle, streamAddr, streamSize);

    if (handle >= CELL_DMUX_MAX_HANDLES || !s_dmux[handle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    /* In a full implementation, this would parse the MPEG-TS stream
       and deliver AU_FOUND callbacks to enabled ES handles.
       For now, notify completion. */
    if (s_dmux[handle].cbFunc)
        s_dmux[handle].cbFunc(handle, CELL_DMUX_MSG_TYPE_DEMUX_DONE,
                               0, s_dmux[handle].cbArg);

    return CELL_OK;
}

s32 cellDmuxResetStream(CellDmuxHandle handle)
{
    printf("[cellDmux] ResetStream(handle=%u)\n", handle);

    if (handle >= CELL_DMUX_MAX_HANDLES || !s_dmux[handle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    /* Clear pending AUs from all ES handles */
    for (int i = 0; i < CELL_DMUX_MAX_ES; i++) {
        if (s_es[i].in_use && s_es[i].dmuxId == handle)
            s_es[i].hasAu = 0;
    }

    return CELL_OK;
}

s32 cellDmuxResetStreamAndWaitDone(CellDmuxHandle handle)
{
    return cellDmuxResetStream(handle);
}

/* ---------------------------------------------------------------------------
 * AU retrieval
 * -----------------------------------------------------------------------*/

s32 cellDmuxGetAu(CellDmuxEsHandle esHandle, CellDmuxAuInfo** auInfo, u32* auInfoNum)
{
    if (esHandle >= CELL_DMUX_MAX_ES || !s_es[esHandle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    if (!s_es[esHandle].hasAu)
        return (s32)CELL_DMUX_ERROR_EMPTY;

    if (auInfo)
        *auInfo = &s_es[esHandle].currentAu;
    if (auInfoNum)
        *auInfoNum = 1;

    return CELL_OK;
}

s32 cellDmuxPeekAu(CellDmuxEsHandle esHandle, CellDmuxAuInfo** auInfo, u32* auInfoNum)
{
    return cellDmuxGetAu(esHandle, auInfo, auInfoNum);
}

s32 cellDmuxReleaseAu(CellDmuxEsHandle esHandle)
{
    if (esHandle >= CELL_DMUX_MAX_ES || !s_es[esHandle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    s_es[esHandle].hasAu = 0;
    return CELL_OK;
}

s32 cellDmuxFlushEs(CellDmuxEsHandle esHandle)
{
    printf("[cellDmux] FlushEs(es=%u)\n", esHandle);

    if (esHandle >= CELL_DMUX_MAX_ES || !s_es[esHandle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    s_es[esHandle].hasAu = 0;

    if (s_es[esHandle].esCbFunc)
        s_es[esHandle].esCbFunc(s_es[esHandle].dmuxId, esHandle,
                                 CELL_DMUX_ES_MSG_TYPE_FLUSH_DONE,
                                 0, s_es[esHandle].esCbArg);

    return CELL_OK;
}

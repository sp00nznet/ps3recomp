/*
 * ps3recomp - sceNpMatching2 HLE implementation
 *
 * Offline stub. Init/term/context management works.
 * All server operations return SERVER_NOT_AVAILABLE.
 */

#include "sceNpMatching2.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_initialized = 0;

typedef struct {
    int in_use;
    int started;
    SceNpMatching2SignalingCallback sigCb;
    void* sigArg;
    SceNpMatching2RequestCallback roomCb;
    void* roomArg;
} M2Context;

static M2Context s_ctx[SCE_NP_MATCHING2_CTX_MAX];

/* ---------------------------------------------------------------------------
 * Lifecycle
 * -----------------------------------------------------------------------*/

s32 sceNpMatching2Init(u32 poolSize, s32 threadPriority, s32 threadStackSize)
{
    (void)poolSize; (void)threadPriority; (void)threadStackSize;
    printf("[sceNpMatching2] Init()\n");

    if (s_initialized)
        return (s32)SCE_NP_MATCHING2_ERROR_ALREADY_INITIALIZED;

    memset(s_ctx, 0, sizeof(s_ctx));
    s_initialized = 1;
    return CELL_OK;
}

s32 sceNpMatching2Term(void)
{
    printf("[sceNpMatching2] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Context management
 * -----------------------------------------------------------------------*/

s32 sceNpMatching2CreateContext(const void* npId, const void* commId,
                                  u32 passPhrase, SceNpMatching2CtxId* ctxId)
{
    (void)npId; (void)commId; (void)passPhrase;
    printf("[sceNpMatching2] CreateContext()\n");

    if (!s_initialized) return (s32)SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    if (!ctxId) return (s32)SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < SCE_NP_MATCHING2_CTX_MAX; i++) {
        if (!s_ctx[i].in_use) {
            memset(&s_ctx[i], 0, sizeof(M2Context));
            s_ctx[i].in_use = 1;
            *ctxId = (u16)i;
            return CELL_OK;
        }
    }
    return (s32)SCE_NP_MATCHING2_ERROR_OUT_OF_MEMORY;
}

s32 sceNpMatching2DestroyContext(SceNpMatching2CtxId ctxId)
{
    printf("[sceNpMatching2] DestroyContext(%u)\n", ctxId);
    if (ctxId >= SCE_NP_MATCHING2_CTX_MAX) return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    s_ctx[ctxId].in_use = 0;
    return CELL_OK;
}

s32 sceNpMatching2ContextStart(SceNpMatching2CtxId ctxId)
{
    printf("[sceNpMatching2] ContextStart(%u)\n", ctxId);
    if (ctxId >= SCE_NP_MATCHING2_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    s_ctx[ctxId].started = 1;
    return CELL_OK;
}

s32 sceNpMatching2ContextStop(SceNpMatching2CtxId ctxId)
{
    printf("[sceNpMatching2] ContextStop(%u)\n", ctxId);
    if (ctxId >= SCE_NP_MATCHING2_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    s_ctx[ctxId].started = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Callbacks
 * -----------------------------------------------------------------------*/

s32 sceNpMatching2RegisterSignalingCallback(SceNpMatching2CtxId ctxId,
                                              SceNpMatching2SignalingCallback cb,
                                              void* arg)
{
    if (ctxId >= SCE_NP_MATCHING2_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    s_ctx[ctxId].sigCb = cb;
    s_ctx[ctxId].sigArg = arg;
    return CELL_OK;
}

s32 sceNpMatching2RegisterRoomEventCallback(SceNpMatching2CtxId ctxId,
                                              SceNpMatching2RequestCallback cb,
                                              void* arg)
{
    if (ctxId >= SCE_NP_MATCHING2_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    s_ctx[ctxId].roomCb = cb;
    s_ctx[ctxId].roomArg = arg;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * All server operations - offline stub
 * -----------------------------------------------------------------------*/

s32 sceNpMatching2SearchLobby(SceNpMatching2CtxId ctxId, const void* optParam,
                                SceNpMatching2RequestId* reqId)
{
    (void)ctxId; (void)optParam; (void)reqId;
    printf("[sceNpMatching2] SearchLobby() - offline\n");
    return (s32)SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE;
}

s32 sceNpMatching2JoinLobby(SceNpMatching2CtxId ctxId, SceNpMatching2LobbyId lobbyId,
                              const void* optParam, SceNpMatching2RequestId* reqId)
{
    (void)ctxId; (void)lobbyId; (void)optParam; (void)reqId;
    return (s32)SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE;
}

s32 sceNpMatching2LeaveLobby(SceNpMatching2CtxId ctxId, SceNpMatching2RequestId* reqId)
{
    (void)ctxId; (void)reqId;
    return CELL_OK;
}

s32 sceNpMatching2CreateRoom(SceNpMatching2CtxId ctxId, const void* optParam,
                               SceNpMatching2RequestId* reqId)
{
    (void)ctxId; (void)optParam; (void)reqId;
    printf("[sceNpMatching2] CreateRoom() - offline\n");
    return (s32)SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE;
}

s32 sceNpMatching2JoinRoom(SceNpMatching2CtxId ctxId, SceNpMatching2RoomId roomId,
                             const void* optParam, SceNpMatching2RequestId* reqId)
{
    (void)ctxId; (void)roomId; (void)optParam; (void)reqId;
    return (s32)SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE;
}

s32 sceNpMatching2LeaveRoom(SceNpMatching2CtxId ctxId, SceNpMatching2RequestId* reqId)
{
    (void)ctxId; (void)reqId;
    return CELL_OK;
}

s32 sceNpMatching2SearchRoom(SceNpMatching2CtxId ctxId, const void* optParam,
                               SceNpMatching2RequestId* reqId)
{
    (void)ctxId; (void)optParam; (void)reqId;
    return (s32)SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE;
}

s32 sceNpMatching2AbortRequest(SceNpMatching2CtxId ctxId, SceNpMatching2RequestId reqId)
{
    (void)ctxId; (void)reqId;
    return CELL_OK;
}

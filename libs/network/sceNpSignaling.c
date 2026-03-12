/*
 * ps3recomp - sceNpSignaling HLE implementation
 *
 * Offline stub. Init/term and context management work.
 * Connection operations return NOT_CONNECTED.
 */

#include "sceNpSignaling.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

static int s_initialized = 0;

typedef struct {
    int in_use;
    SceNpSignalingHandler handler;
    void* handlerArg;
} SignalingCtx;

static SignalingCtx s_ctx[SCE_NP_SIGNALING_CTX_MAX];

/* Lifecycle */

s32 sceNpSignalingInit(void)
{
    printf("[sceNpSignaling] Init()\n");
    if (s_initialized)
        return (s32)SCE_NP_SIGNALING_ERROR_ALREADY_INITIALIZED;
    memset(s_ctx, 0, sizeof(s_ctx));
    s_initialized = 1;
    return CELL_OK;
}

s32 sceNpSignalingTerm(void)
{
    printf("[sceNpSignaling] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

/* Context management */

s32 sceNpSignalingCreateCtx(const void* npId, SceNpSignalingHandler handler,
                              void* arg, SceNpSignalingCtxId* ctxId)
{
    (void)npId;
    printf("[sceNpSignaling] CreateCtx()\n");

    if (!s_initialized) return (s32)SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED;
    if (!ctxId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < SCE_NP_SIGNALING_CTX_MAX; i++) {
        if (!s_ctx[i].in_use) {
            memset(&s_ctx[i], 0, sizeof(SignalingCtx));
            s_ctx[i].in_use = 1;
            s_ctx[i].handler = handler;
            s_ctx[i].handlerArg = arg;
            *ctxId = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)SCE_NP_SIGNALING_ERROR_OUT_OF_MEMORY;
}

s32 sceNpSignalingDestroyCtx(SceNpSignalingCtxId ctxId)
{
    printf("[sceNpSignaling] DestroyCtx(%u)\n", ctxId);
    if (ctxId >= SCE_NP_SIGNALING_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    s_ctx[ctxId].in_use = 0;
    return CELL_OK;
}

/* Connection operations — offline stubs */

s32 sceNpSignalingActivateConnection(SceNpSignalingCtxId ctxId,
                                       const void* peerNpId,
                                       SceNpSignalingConnId* connId)
{
    (void)ctxId; (void)peerNpId; (void)connId;
    printf("[sceNpSignaling] ActivateConnection() - offline\n");
    return (s32)SCE_NP_SIGNALING_ERROR_NOT_CONNECTED;
}

s32 sceNpSignalingDeactivateConnection(SceNpSignalingCtxId ctxId,
                                         SceNpSignalingConnId connId)
{
    (void)ctxId; (void)connId;
    return CELL_OK;
}

s32 sceNpSignalingGetConnectionStatus(SceNpSignalingCtxId ctxId,
                                        SceNpSignalingConnId connId,
                                        s32* connStatus,
                                        u32* peerAddr, u16* peerPort)
{
    (void)ctxId; (void)connId;
    if (connStatus) *connStatus = SCE_NP_SIGNALING_CONN_STATUS_INACTIVE;
    if (peerAddr) *peerAddr = 0;
    if (peerPort) *peerPort = 0;
    return CELL_OK;
}

s32 sceNpSignalingGetConnectionInfo(SceNpSignalingCtxId ctxId,
                                      SceNpSignalingConnId connId,
                                      SceNpSignalingConnectionInfo* info)
{
    (void)ctxId; (void)connId;
    if (!info) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    memset(info, 0, sizeof(SceNpSignalingConnectionInfo));
    info->status = SCE_NP_SIGNALING_CONN_STATUS_INACTIVE;
    return CELL_OK;
}

s32 sceNpSignalingGetLocalNetInfo(u32* localAddr, u16* localPort)
{
    if (localAddr) *localAddr = 0x7F000001; /* 127.0.0.1 */
    if (localPort) *localPort = 0;
    return CELL_OK;
}

s32 sceNpSignalingGetPeerNetInfo(SceNpSignalingCtxId ctxId,
                                   SceNpSignalingConnId connId,
                                   u32* peerAddr, u16* peerPort)
{
    (void)ctxId; (void)connId;
    if (peerAddr) *peerAddr = 0;
    if (peerPort) *peerPort = 0;
    return CELL_OK;
}

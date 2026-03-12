/*
 * ps3recomp - cellRudp HLE implementation
 *
 * Offline stub. Context management works, connect/send return errors.
 */

#include "cellRudp.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

static int s_initialized = 0;

typedef struct {
    int in_use;
    s32 state;
    CellRudpEventHandler handler;
    void* handlerArg;
} RudpCtx;

static RudpCtx s_ctx[CELL_RUDP_CTX_MAX];

/* API */

s32 cellRudpInit(u32 maxContexts)
{
    (void)maxContexts;
    printf("[cellRudp] Init()\n");
    if (s_initialized)
        return (s32)CELL_RUDP_ERROR_ALREADY_INITIALIZED;
    memset(s_ctx, 0, sizeof(s_ctx));
    s_initialized = 1;
    return CELL_OK;
}

s32 cellRudpEnd(void)
{
    printf("[cellRudp] End()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellRudpCreateContext(const CellRudpOption* option,
                            CellRudpEventHandler handler,
                            void* arg, CellRudpCtxId* ctxId)
{
    (void)option;
    printf("[cellRudp] CreateContext()\n");

    if (!s_initialized) return (s32)CELL_RUDP_ERROR_NOT_INITIALIZED;
    if (!ctxId) return (s32)CELL_RUDP_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < CELL_RUDP_CTX_MAX; i++) {
        if (!s_ctx[i].in_use) {
            memset(&s_ctx[i], 0, sizeof(RudpCtx));
            s_ctx[i].in_use = 1;
            s_ctx[i].state = CELL_RUDP_STATE_IDLE;
            s_ctx[i].handler = handler;
            s_ctx[i].handlerArg = arg;
            *ctxId = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)CELL_RUDP_ERROR_OUT_OF_MEMORY;
}

s32 cellRudpDestroyContext(CellRudpCtxId ctxId)
{
    printf("[cellRudp] DestroyContext(%u)\n", ctxId);
    if (ctxId >= CELL_RUDP_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)CELL_RUDP_ERROR_CTX_NOT_FOUND;
    s_ctx[ctxId].in_use = 0;
    return CELL_OK;
}

s32 cellRudpBind(CellRudpCtxId ctxId, u16 port)
{
    (void)port;
    if (ctxId >= CELL_RUDP_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)CELL_RUDP_ERROR_CTX_NOT_FOUND;
    return CELL_OK;
}

s32 cellRudpConnect(CellRudpCtxId ctxId, u32 addr, u16 port)
{
    (void)addr; (void)port;
    printf("[cellRudp] Connect() - offline\n");
    if (ctxId >= CELL_RUDP_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)CELL_RUDP_ERROR_CTX_NOT_FOUND;
    return (s32)CELL_RUDP_ERROR_NOT_CONNECTED;
}

s32 cellRudpClose(CellRudpCtxId ctxId)
{
    if (ctxId >= CELL_RUDP_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)CELL_RUDP_ERROR_CTX_NOT_FOUND;
    s_ctx[ctxId].state = CELL_RUDP_STATE_IDLE;
    return CELL_OK;
}

s32 cellRudpSend(CellRudpCtxId ctxId, const void* data, u32 size)
{
    (void)data; (void)size;
    if (ctxId >= CELL_RUDP_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)CELL_RUDP_ERROR_CTX_NOT_FOUND;
    return (s32)CELL_RUDP_ERROR_NOT_CONNECTED;
}

s32 cellRudpRecv(CellRudpCtxId ctxId, void* data, u32 size, u32* received)
{
    (void)data; (void)size;
    if (ctxId >= CELL_RUDP_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)CELL_RUDP_ERROR_CTX_NOT_FOUND;
    if (received) *received = 0;
    return (s32)CELL_RUDP_ERROR_NOT_CONNECTED;
}

s32 cellRudpGetState(CellRudpCtxId ctxId, s32* state)
{
    if (ctxId >= CELL_RUDP_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)CELL_RUDP_ERROR_CTX_NOT_FOUND;
    if (!state) return (s32)CELL_RUDP_ERROR_INVALID_ARGUMENT;
    *state = s_ctx[ctxId].state;
    return CELL_OK;
}

s32 cellRudpFlush(CellRudpCtxId ctxId)
{
    if (ctxId >= CELL_RUDP_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)CELL_RUDP_ERROR_CTX_NOT_FOUND;
    return CELL_OK;
}

s32 cellRudpPoll(CellRudpCtxId ctxId, s32 events, s32* revents, u32 timeoutMs)
{
    (void)events; (void)timeoutMs;
    if (ctxId >= CELL_RUDP_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)CELL_RUDP_ERROR_CTX_NOT_FOUND;
    if (revents) *revents = 0;
    return CELL_OK;
}

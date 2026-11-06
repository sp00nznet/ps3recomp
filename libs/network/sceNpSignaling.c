/*
 * ps3recomp - sceNpSignaling HLE implementation
 *
 * Contexts, handlers and the connections to Matching2 room members. A
 * connection comes up when sceNpMatching2.c reports the member up
 * (np_signaling_room_peer) and is mutual once the title has also activated it;
 * events go to each context's handler and extended handler with the SDK values.
 * Connections to peers outside a room stay PENDING. Nothing is sent from here:
 * the title's own P2P sockets carry the traffic.
 */

#include "sceNpSignaling.h"
#include <stdio.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../system/cellSysutil.h"          /* cellSysutilQueueGuestCallbackArgs */

/* Internal state */

static int s_initialized = 0;

typedef struct {
    int in_use;
    SceNpSignalingHandler handler;
    void* handlerArg;
    SceNpSignalingHandler extHandler;   /* sceNpSignalingAddExtendedHandler (guest descriptor) */
    void* extHandlerArg;
} SignalingCtx;

static SignalingCtx s_ctx[SCE_NP_SIGNALING_CTX_MAX];

/* Context ids are slot + 1: titles treat id 0 as "no context". J-Stars Victory VS+ activates a room member's
 * connection only when its context id is nonzero; with the first context numbered 0 it never activated one. */
static SignalingCtx* ctx_get(SceNpSignalingCtxId id)
{
    return (id >= 1 && id <= SCE_NP_SIGNALING_CTX_MAX && s_ctx[id - 1].in_use) ? &s_ctx[id - 1] : NULL;
}

/* Connections, one per peer (by online id), shared by all contexts; conn id = index + 1. Only room members made by
 * Matching2 come up for now: their address is the one psnr gives for the member's P2P socket. */
typedef struct {
    int used;
    char online_id[17];
    u8 ip[4];
    u16 port;
    s32 status;   /* SCE_NP_SIGNALING_CONN_STATUS_* */
    int activated;   /* the title called ActivateConnection for this peer */
    int mutual;      /* EXT_MUTUAL_ACTIVATED sent */
} SignalingConn;

static SignalingConn s_conn[SCE_NP_SIGNALING_CONN_MAX];

static SignalingConn* conn_get(SceNpSignalingConnId id)
{
    return (id >= 1 && id <= SCE_NP_SIGNALING_CONN_MAX && s_conn[id - 1].used) ? &s_conn[id - 1] : NULL;
}

static u32 conn_addr(const SignalingConn* c)   /* in_addr, network order as a guest u32 */
{
    return ((u32)c->ip[0] << 24) | ((u32)c->ip[1] << 16) | ((u32)c->ip[2] << 8) | c->ip[3];
}

/* Every context's handler and extended handler: (ctxId, connId, event, errorCode, arg). The extended handler also
 * receives the normal events. Delivered on the title's next cellSysutilCheckCallback. */
static void conn_event(SceNpSignalingConnId id, s32 event, s32 err)
{
    const int ext_only = event >= SCE_NP_SIGNALING_EVENT_EXT_PEER_ACTIVATED;
    for (u32 i = 0; i < SCE_NP_SIGNALING_CTX_MAX; i++) {
        if (!s_ctx[i].in_use) continue;
        const u32 h[2] = { (u32)(uintptr_t)s_ctx[i].handler, (u32)(uintptr_t)s_ctx[i].extHandler };
        const u32 a[2] = { (u32)(uintptr_t)s_ctx[i].handlerArg, (u32)(uintptr_t)s_ctx[i].extHandlerArg };
        for (int k = ext_only; k < 2; k++) {
            if (!h[k]) continue;
            const u64 args[8] = { i + 1, id, (u64)(u32)event, (u64)(u32)err, a[k], 0, 0, 0 };
            cellSysutilQueueGuestCallbackArgs(h[k], args);
        }
    }
}

/* The connection to a peer by online id; create = make one (status INACTIVE) if there is none. Index or -1. */
static int conn_find(const char* online_id, int create)
{
    int freei = -1;
    for (int i = 0; i < SCE_NP_SIGNALING_CONN_MAX; i++) {
        if (s_conn[i].used && strcmp(s_conn[i].online_id, online_id) == 0) return i;
        if (!s_conn[i].used && freei < 0) freei = i;
    }
    if (!create || freei < 0) return -1;
    memset(&s_conn[freei], 0, sizeof(s_conn[freei]));
    s_conn[freei].used = 1;
    strncpy(s_conn[freei].online_id, online_id, 16);
    return freei;
}

/* Both ends active: the peer's side is up (room member) and this side activated it. The extended handler hears that
 * the peer activated the connection and that it is mutual; J-Stars Victory VS+ marks a room member connected only
 * on EXT_MUTUAL_ACTIVATED (on ESTABLISHED it only logs). */
static void conn_check_mutual(int idx)
{
    SignalingConn* c = &s_conn[idx];
    if (c->mutual || !c->activated || c->status != SCE_NP_SIGNALING_CONN_STATUS_ACTIVE) return;
    c->mutual = 1;
    conn_event((SceNpSignalingConnId)(idx + 1), SCE_NP_SIGNALING_EVENT_EXT_PEER_ACTIVATED, 0);
    conn_event((SceNpSignalingConnId)(idx + 1), SCE_NP_SIGNALING_EVENT_EXT_MUTUAL_ACTIVATED, 0);
}

void np_signaling_room_peer(const char* online_id, const u8 ip[4], u16 port, int up)
{
    const int idx = conn_find(online_id, up);
    if (idx < 0) return;
    SignalingConn* c = &s_conn[idx];
    const s32 want = up ? SCE_NP_SIGNALING_CONN_STATUS_ACTIVE : SCE_NP_SIGNALING_CONN_STATUS_INACTIVE;
    if (up) { memcpy(c->ip, ip, 4); c->port = port; }
    if (c->status == want) return;
    c->status = want;
    printf("[sceNpSignaling] conn %d (%s, %u.%u.%u.%u:%u) %s\n", idx + 1, c->online_id, c->ip[0], c->ip[1], c->ip[2],
           c->ip[3], c->port, up ? "established" : "dead");
    conn_event((SceNpSignalingConnId)(idx + 1), up ? SCE_NP_SIGNALING_EVENT_ESTABLISHED : SCE_NP_SIGNALING_EVENT_DEAD, 0);
    if (up) conn_check_mutual(idx);
    else c->activated = c->mutual = 0;
}

/* sceNpSignalingInit is not a firmware export (no NID): on the console signaling is usable once sceNpInit has run,
 * so a title calls sceNpSignalingCreateCtx straight after sceNpInit. Accept either, as sceNpBasic does. */
int np_is_initialized(void);
static int signaling_ready(void) { return s_initialized || np_is_initialized(); }

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

    if (!signaling_ready()) return (s32)SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED;
    if (!ctxId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < SCE_NP_SIGNALING_CTX_MAX; i++) {
        if (!s_ctx[i].in_use) {
            memset(&s_ctx[i], 0, sizeof(SignalingCtx));
            s_ctx[i].in_use = 1;
            s_ctx[i].handler = handler;
            s_ctx[i].handlerArg = arg;
            vm_write32((u32)(uintptr_t)ctxId, (u32)i + 1);
            return CELL_OK;
        }
    }
    return (s32)SCE_NP_SIGNALING_ERROR_OUT_OF_MEMORY;
}

s32 sceNpSignalingDestroyCtx(SceNpSignalingCtxId ctxId)
{
    printf("[sceNpSignaling] DestroyCtx(%u)\n", ctxId);
    SignalingCtx* c = ctx_get(ctxId);
    if (!c) return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    c->in_use = 0;
    return CELL_OK;
}

/* The extended handler receives the extended events (peer activated, ...) besides the ones the CreateCtx handler
 * gets. A title may create the context with a null handler and register this one instead. Stored
 * here; room members' connections (np_signaling_room_peer) raise its events. */
s32 sceNpSignalingAddExtendedHandler(SceNpSignalingCtxId ctxId, SceNpSignalingHandler handler, void* arg)
{
    printf("[sceNpSignaling] AddExtendedHandler(ctx=%u, handler=0x%08X)\n", ctxId, (u32)(uintptr_t)handler);
    if (!signaling_ready()) return (s32)SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED;
    SignalingCtx* c = ctx_get(ctxId);
    if (!c) return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    c->extHandler = handler;
    c->extHandlerArg = arg;
    return CELL_OK;
}

/* Connection operations — offline stubs */

s32 sceNpSignalingActivateConnection(SceNpSignalingCtxId ctxId,
                                       const void* peerNpId,
                                       SceNpSignalingConnId* connId)
{
    if (!ctx_get(ctxId)) return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    if (!peerNpId || !connId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    char id[17] = {0};
    for (u32 o = 0; o < 16; o++) id[o] = (char)vm_read8((u32)(uintptr_t)peerNpId + o);
    const int idx = conn_find(id, 1);
    if (idx < 0) return (s32)SCE_NP_SIGNALING_ERROR_OUT_OF_MEMORY;
    SignalingConn* c = &s_conn[idx];
    if (c->status == SCE_NP_SIGNALING_CONN_STATUS_INACTIVE) c->status = SCE_NP_SIGNALING_CONN_STATUS_PENDING;
    c->activated = 1;
    vm_write32((u32)(uintptr_t)connId, (u32)(idx + 1));
    printf("[sceNpSignaling] ActivateConnection(ctx=%u, %s) -> conn %d (%s)\n", ctxId, id, idx + 1,
           c->status == SCE_NP_SIGNALING_CONN_STATUS_ACTIVE ? "active" : "pending");
    if (c->status == SCE_NP_SIGNALING_CONN_STATUS_ACTIVE) {   /* already up: say so, as a new activation would */
        conn_event((SceNpSignalingConnId)(idx + 1), SCE_NP_SIGNALING_EVENT_ESTABLISHED, 0);
        conn_check_mutual(idx);
    }
    return CELL_OK;
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
    (void)ctxId;
    const SignalingConn* c = conn_get(connId);
    if (connStatus) vm_write32((u32)(uintptr_t)connStatus, (u32)(c ? c->status : SCE_NP_SIGNALING_CONN_STATUS_INACTIVE));
    if (peerAddr) vm_write32((u32)(uintptr_t)peerAddr, c ? conn_addr(c) : 0u);
    if (peerPort) vm_write16((u32)(uintptr_t)peerPort, (u16)(c ? c->port : 0));
    return c ? CELL_OK : (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND;
}

s32 sceNpSignalingGetConnectionInfo(SceNpSignalingCtxId ctxId,
                                      SceNpSignalingConnId connId,
                                      s32 code, void* info)
{
    (void)ctxId;
    if (!info) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    const SignalingConn* c = conn_get(connId);
    if (!c) return (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND;
    const u32 ea = (u32)(uintptr_t)info;
    switch (code) {
    case SCE_NP_SIGNALING_CONN_INFO_RTT:         vm_write32(ea, 1000); break;        /* us; loopback/LAN */
    case SCE_NP_SIGNALING_CONN_INFO_BANDWIDTH:   vm_write32(ea, 10000000); break;    /* bits/s */
    case SCE_NP_SIGNALING_CONN_INFO_PACKET_LOSS: vm_write32(ea, 0); break;
    case SCE_NP_SIGNALING_CONN_INFO_PEER_NPID:   /* SceNpId: SceNpOnlineId { char data[16]; term; dummy[3] }, opt[8], reserved[8] */
        for (u32 o = 0; o < 36; o++) vm_write8(ea + o, 0);
        for (u32 o = 0; o < 16 && c->online_id[o]; o++) vm_write8(ea + o, (u8)c->online_id[o]);
        break;
    case SCE_NP_SIGNALING_CONN_INFO_PEER_ADDRESS:
    case SCE_NP_SIGNALING_CONN_INFO_MAPPED_ADDRESS:
        vm_write32(ea, conn_addr(c));
        vm_write16(ea + 4, c->port);
        break;
    default: return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    }
    return CELL_OK;
}

s32 sceNpSignalingGetConnectionFromNpId(SceNpSignalingCtxId ctxId, const void* npId,
                                          SceNpSignalingConnId* connId)
{
    (void)ctxId;
    if (!npId || !connId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    char id[17] = {0};
    for (u32 o = 0; o < 16; o++) id[o] = (char)vm_read8((u32)(uintptr_t)npId + o);
    for (int i = 0; i < SCE_NP_SIGNALING_CONN_MAX; i++)
        if (s_conn[i].used && strcmp(s_conn[i].online_id, id) == 0) {
            vm_write32((u32)(uintptr_t)connId, (u32)(i + 1));
            return CELL_OK;
        }
    return (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND;
}

s32 sceNpSignalingGetLocalNetInfo(u32* localAddr, u16* localPort)
{
    if (localAddr) vm_write32((u32)(uintptr_t)localAddr, (u32)0x7F000001); /* 127.0.0.1 */
    if (localPort) vm_write16((u32)(uintptr_t)localPort, (u16)0);
    return CELL_OK;
}

s32 sceNpSignalingGetPeerNetInfo(SceNpSignalingCtxId ctxId,
                                   SceNpSignalingConnId connId,
                                   u32* peerAddr, u16* peerPort)
{
    (void)ctxId;
    const SignalingConn* c = conn_get(connId);
    if (peerAddr) vm_write32((u32)(uintptr_t)peerAddr, c ? conn_addr(c) : 0u);
    if (peerPort) vm_write16((u32)(uintptr_t)peerPort, (u16)(c ? c->port : 0));
    return c ? CELL_OK : (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND;
}

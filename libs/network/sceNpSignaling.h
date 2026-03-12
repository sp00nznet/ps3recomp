/*
 * ps3recomp - sceNpSignaling HLE
 *
 * P2P connection signaling for NAT traversal and direct player connections.
 * Offline stub — returns NOT_CONNECTED for connection operations.
 */

#ifndef PS3RECOMP_SCE_NP_SIGNALING_H
#define PS3RECOMP_SCE_NP_SIGNALING_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED       0x80022A01
#define SCE_NP_SIGNALING_ERROR_ALREADY_INITIALIZED   0x80022A02
#define SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT      0x80022A03
#define SCE_NP_SIGNALING_ERROR_OUT_OF_MEMORY         0x80022A04
#define SCE_NP_SIGNALING_ERROR_NOT_CONNECTED         0x80022A05
#define SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND         0x80022A06
#define SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND        0x80022A07

/* Constants */
#define SCE_NP_SIGNALING_CTX_MAX           8
#define SCE_NP_SIGNALING_CONN_MAX          16

/* Connection status */
#define SCE_NP_SIGNALING_CONN_STATUS_INACTIVE    0
#define SCE_NP_SIGNALING_CONN_STATUS_PENDING     1
#define SCE_NP_SIGNALING_CONN_STATUS_ACTIVE      2

/* Event types */
#define SCE_NP_SIGNALING_EVENT_CONNECTED         1
#define SCE_NP_SIGNALING_EVENT_DEAD              2
#define SCE_NP_SIGNALING_EVENT_ESTABLISHED       3

/* Types */
typedef u32 SceNpSignalingCtxId;
typedef u32 SceNpSignalingConnId;

typedef struct SceNpSignalingConnectionInfo {
    u32 address;
    u16 port;
    u16 padding;
    u32 status;
    u32 peerAddress;
    u16 peerPort;
    u16 reserved;
} SceNpSignalingConnectionInfo;

typedef void (*SceNpSignalingHandler)(SceNpSignalingCtxId ctxId,
                                       SceNpSignalingConnId connId,
                                       u32 event, s32 errorCode,
                                       void* arg);

/* Functions */
s32 sceNpSignalingInit(void);
s32 sceNpSignalingTerm(void);

s32 sceNpSignalingCreateCtx(const void* npId, SceNpSignalingHandler handler,
                              void* arg, SceNpSignalingCtxId* ctxId);
s32 sceNpSignalingDestroyCtx(SceNpSignalingCtxId ctxId);

s32 sceNpSignalingActivateConnection(SceNpSignalingCtxId ctxId,
                                       const void* peerNpId,
                                       SceNpSignalingConnId* connId);
s32 sceNpSignalingDeactivateConnection(SceNpSignalingCtxId ctxId,
                                         SceNpSignalingConnId connId);

s32 sceNpSignalingGetConnectionStatus(SceNpSignalingCtxId ctxId,
                                        SceNpSignalingConnId connId,
                                        s32* connStatus,
                                        u32* peerAddr, u16* peerPort);

s32 sceNpSignalingGetConnectionInfo(SceNpSignalingCtxId ctxId,
                                      SceNpSignalingConnId connId,
                                      SceNpSignalingConnectionInfo* info);

s32 sceNpSignalingGetLocalNetInfo(u32* localAddr, u16* localPort);
s32 sceNpSignalingGetPeerNetInfo(SceNpSignalingCtxId ctxId,
                                   SceNpSignalingConnId connId,
                                   u32* peerAddr, u16* peerPort);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_SIGNALING_H */

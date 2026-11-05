/*
 * ps3recomp - sceNpSignaling HLE
 *
 * P2P connection signaling for NAT traversal and direct player connections.
 * Connections are those of Matching2 room members (see sceNpSignaling.c).
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

/* Event types (SDK values) */
#define SCE_NP_SIGNALING_EVENT_DEAD                 0
#define SCE_NP_SIGNALING_EVENT_ESTABLISHED          1
#define SCE_NP_SIGNALING_EVENT_NETINFO_ERROR        2
#define SCE_NP_SIGNALING_EVENT_NETINFO_RESULT       3
#define SCE_NP_SIGNALING_EVENT_EXT_PEER_ACTIVATED   10
#define SCE_NP_SIGNALING_EVENT_EXT_PEER_DEACTIVATED 11
#define SCE_NP_SIGNALING_EVENT_EXT_MUTUAL_ACTIVATED 12

/* sceNpSignalingGetConnectionInfo codes: which member of the info union is written */
#define SCE_NP_SIGNALING_CONN_INFO_RTT            1
#define SCE_NP_SIGNALING_CONN_INFO_BANDWIDTH      2
#define SCE_NP_SIGNALING_CONN_INFO_PEER_NPID      3
#define SCE_NP_SIGNALING_CONN_INFO_PEER_ADDRESS   4
#define SCE_NP_SIGNALING_CONN_INFO_MAPPED_ADDRESS 5
#define SCE_NP_SIGNALING_CONN_INFO_PACKET_LOSS    6

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
s32 sceNpSignalingAddExtendedHandler(SceNpSignalingCtxId ctxId, SceNpSignalingHandler handler,
                                      void* arg);

s32 sceNpSignalingActivateConnection(SceNpSignalingCtxId ctxId,
                                       const void* peerNpId,
                                       SceNpSignalingConnId* connId);
s32 sceNpSignalingDeactivateConnection(SceNpSignalingCtxId ctxId,
                                         SceNpSignalingConnId connId);

s32 sceNpSignalingGetConnectionStatus(SceNpSignalingCtxId ctxId,
                                        SceNpSignalingConnId connId,
                                        s32* connStatus,
                                        u32* peerAddr, u16* peerPort);

/* info is the SDK's union SceNpSignalingConnectionInfo { u32 rtt; u32 bandwidth; SceNpId npId;
 * struct { in_addr addr; in_port_t port; } address; u32 packet_loss; }, the member chosen by code. */
s32 sceNpSignalingGetConnectionInfo(SceNpSignalingCtxId ctxId,
                                      SceNpSignalingConnId connId,
                                      s32 code, void* info);
s32 sceNpSignalingGetConnectionFromNpId(SceNpSignalingCtxId ctxId, const void* npId,
                                          SceNpSignalingConnId* connId);

/* A Matching2 room member's connection came up (up = 1) or went down: on the console Matching2 connects room
 * members through this library, so the connection has a sceNpSignaling id and its events reach the contexts'
 * handlers too. Called by sceNpMatching2.c; ip is a.b.c.d, port the member's P2P port. */
void np_signaling_room_peer(const char* online_id, const u8 ip[4], u16 port, int up);

s32 sceNpSignalingGetLocalNetInfo(u32* localAddr, u16* localPort);
s32 sceNpSignalingGetPeerNetInfo(SceNpSignalingCtxId ctxId,
                                   SceNpSignalingConnId connId,
                                   u32* peerAddr, u16* peerPort);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_SIGNALING_H */

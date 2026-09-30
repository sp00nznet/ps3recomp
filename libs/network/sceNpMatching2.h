/*
 * ps3recomp - sceNpMatching2 HLE
 *
 * Rooms over a psnr server (np_psnr.h). Offline -- no PSNR_SERVER -- every
 * server request fails with SERVER_NOT_AVAILABLE, as before.
 *
 * Pointer parameters are guest addresses, passed as u32. Structure layouts
 * are the SDK ABI; see the offset comments in sceNpMatching2.c.
 */

#ifndef PS3RECOMP_SCE_NP_MATCHING2_H
#define PS3RECOMP_SCE_NP_MATCHING2_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED       0x80022C01
#define SCE_NP_MATCHING2_ERROR_ALREADY_INITIALIZED   0x80022C02
#define SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT      0x80022C03
#define SCE_NP_MATCHING2_ERROR_OUT_OF_MEMORY         0x80022C04
#define SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE  0x80022C05
#define SCE_NP_MATCHING2_ERROR_NOT_CONNECTED         0x80022C06
#define SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND     0x80022C07
#define SCE_NP_MATCHING2_ERROR_EVENT_DATA_NOT_FOUND  0x80022C08

/* Server-side errors delivered in callbacks. */
#define SCE_NP_MATCHING2_SERVER_ERROR_BAD_REQUEST    0x80022B01
#define SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_ROOM   0x80022B13
#define SCE_NP_MATCHING2_SERVER_ERROR_ROOM_FULL      0x80022B19
#define SCE_NP_MATCHING2_SERVER_ERROR_FORBIDDEN      0x80022B07

#define SCE_NP_MATCHING2_CTX_MAX 8

typedef u16 SceNpMatching2ContextId;
typedef u32 SceNpMatching2RequestId;
typedef u64 SceNpMatching2RoomId;

/* Lifecycle */
s32 sceNp2Init(u32 poolSize, u32 pool);
s32 sceNp2Term(void);
s32 sceNpMatching2Init(u32 poolSize, s32 threadPriority, s32 threadStackSize);
s32 sceNpMatching2Init2(u32 stackSize, s32 priority, u32 param);
s32 sceNpMatching2Term(void);
s32 sceNpMatching2Term2(void);

/* Contexts */
s32 sceNpMatching2CreateContext(u32 npId, u32 commId, u32 passPhrase, u32 ctxId, s32 option);
s32 sceNpMatching2DestroyContext(u16 ctxId);
s32 sceNpMatching2ContextStart(u16 ctxId);
s32 sceNpMatching2ContextStartAsync(u16 ctxId, u32 timeout);
s32 sceNpMatching2ContextStop(u16 ctxId);
s32 sceNpMatching2ContextStopAsync(u16 ctxId, u32 timeout);
s32 sceNpMatching2SetDefaultRequestOptParam(u16 ctxId, u32 optParam);

/* Callbacks */
s32 sceNpMatching2RegisterContextCallback(u16 ctxId, u32 cb, u32 arg);
s32 sceNpMatching2RegisterRoomEventCallback(u16 ctxId, u32 cb, u32 arg);
s32 sceNpMatching2RegisterRoomMessageCallback(u16 ctxId, u32 cb, u32 arg);
s32 sceNpMatching2RegisterSignalingCallback(u16 ctxId, u32 cb, u32 arg);
s32 sceNpMatching2RegisterLobbyEventCallback(u16 ctxId, u32 cb, u32 arg);
s32 sceNpMatching2RegisterLobbyMessageCallback(u16 ctxId, u32 cb, u32 arg);

/* Server and world (one of each, answered locally) */
s32 sceNpMatching2GetServerIdListLocal(u16 ctxId, u32 serverId, u32 maxNum);
s32 sceNpMatching2GetServerInfo(u16 ctxId, u32 req, u32 opt, u32 reqId);
s32 sceNpMatching2CreateServerContext(u16 ctxId, u32 req, u32 opt, u32 reqId);
s32 sceNpMatching2DeleteServerContext(u16 ctxId, u32 req, u32 opt, u32 reqId);
s32 sceNpMatching2GetWorldInfoList(u16 ctxId, u32 req, u32 opt, u32 reqId);

/* Rooms */
s32 sceNpMatching2SearchRoom(u16 ctxId, u32 req, u32 opt, u32 reqId);
s32 sceNpMatching2CreateJoinRoom(u16 ctxId, u32 req, u32 opt, u32 reqId);
s32 sceNpMatching2JoinRoom(u16 ctxId, u32 req, u32 opt, u32 reqId);
s32 sceNpMatching2LeaveRoom(u16 ctxId, u32 req, u32 opt, u32 reqId);
s32 sceNpMatching2KickoutRoomMember(u16 ctxId, u32 req, u32 opt, u32 reqId);
s32 sceNpMatching2SetRoomDataExternal(u16 ctxId, u32 req, u32 opt, u32 reqId);
s32 sceNpMatching2SetRoomDataInternal(u16 ctxId, u32 req, u32 opt, u32 reqId);
s32 sceNpMatching2SendRoomMessage(u16 ctxId, u32 req, u32 opt, u32 reqId);
s32 sceNpMatching2GetEventData(u16 ctxId, u32 eventKey, u32 buf, u32 bufLen);
s32 sceNpMatching2AbortRequest(u16 ctxId, u32 reqId);

/* Signaling (mesh, established as members join) */
s32 sceNpMatching2SignalingGetConnectionStatus(u16 ctxId, u64 roomId, u16 memberId,
                                               u32 connStatus, u32 peerAddr, u32 peerPort);
s32 sceNpMatching2SignalingGetPingInfo(u16 ctxId, u32 req, u32 opt, u32 reqId);

/* Not SDK exports. Kept so NID tables generated against the old stub still link. */
s32 sceNpMatching2SearchLobby(u16 ctxId, u32 opt, u32 reqId);
s32 sceNpMatching2JoinLobby(u16 ctxId, u64 lobbyId, u32 opt, u32 reqId);
s32 sceNpMatching2LeaveLobby(u16 ctxId, u32 reqId);
s32 sceNpMatching2CreateRoom(u16 ctxId, u32 opt, u32 reqId);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_MATCHING2_H */

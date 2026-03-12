/*
 * ps3recomp - sceNpMatching2 HLE
 *
 * Online matchmaking: lobbies, rooms, signaling, and session management.
 * Offline stub — returns server unavailable for all operations.
 */

#ifndef PS3RECOMP_SCE_NP_MATCHING2_H
#define PS3RECOMP_SCE_NP_MATCHING2_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED       0x80022C01
#define SCE_NP_MATCHING2_ERROR_ALREADY_INITIALIZED   0x80022C02
#define SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT      0x80022C03
#define SCE_NP_MATCHING2_ERROR_OUT_OF_MEMORY         0x80022C04
#define SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE   0x80022C05
#define SCE_NP_MATCHING2_ERROR_NOT_CONNECTED         0x80022C06
#define SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND     0x80022C07

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define SCE_NP_MATCHING2_CTX_MAX         8
#define SCE_NP_MATCHING2_LOBBY_MAX       16
#define SCE_NP_MATCHING2_ROOM_MAX        16

/* Callback event types */
#define SCE_NP_MATCHING2_EVENT_SignalingOptParam    1
#define SCE_NP_MATCHING2_EVENT_RoomMessage          2
#define SCE_NP_MATCHING2_EVENT_RoomMemberUpdate     3
#define SCE_NP_MATCHING2_EVENT_LobbyMessage         4

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef u16 SceNpMatching2CtxId;
typedef u32 SceNpMatching2RequestId;
typedef u64 SceNpMatching2RoomId;
typedef u64 SceNpMatching2LobbyId;

typedef void (*SceNpMatching2RequestCallback)(SceNpMatching2CtxId ctxId,
                                                SceNpMatching2RequestId reqId,
                                                u16 event, s32 errorCode,
                                                const void* data, void* arg);

typedef void (*SceNpMatching2SignalingCallback)(SceNpMatching2CtxId ctxId,
                                                  SceNpMatching2RoomId roomId,
                                                  u16 event,
                                                  s32 errorCode,
                                                  const void* data, void* arg);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* Lifecycle */
s32 sceNpMatching2Init(u32 poolSize, s32 threadPriority, s32 threadStackSize);
s32 sceNpMatching2Term(void);

/* Context management */
s32 sceNpMatching2CreateContext(const void* npId, const void* commId,
                                  u32 passPhrase, SceNpMatching2CtxId* ctxId);
s32 sceNpMatching2DestroyContext(SceNpMatching2CtxId ctxId);
s32 sceNpMatching2ContextStart(SceNpMatching2CtxId ctxId);
s32 sceNpMatching2ContextStop(SceNpMatching2CtxId ctxId);

/* Callbacks */
s32 sceNpMatching2RegisterSignalingCallback(SceNpMatching2CtxId ctxId,
                                              SceNpMatching2SignalingCallback cb,
                                              void* arg);
s32 sceNpMatching2RegisterRoomEventCallback(SceNpMatching2CtxId ctxId,
                                              SceNpMatching2RequestCallback cb,
                                              void* arg);

/* Lobby operations */
s32 sceNpMatching2SearchLobby(SceNpMatching2CtxId ctxId,
                                const void* optParam,
                                SceNpMatching2RequestId* reqId);
s32 sceNpMatching2JoinLobby(SceNpMatching2CtxId ctxId,
                              SceNpMatching2LobbyId lobbyId,
                              const void* optParam,
                              SceNpMatching2RequestId* reqId);
s32 sceNpMatching2LeaveLobby(SceNpMatching2CtxId ctxId,
                               SceNpMatching2RequestId* reqId);

/* Room operations */
s32 sceNpMatching2CreateRoom(SceNpMatching2CtxId ctxId,
                               const void* optParam,
                               SceNpMatching2RequestId* reqId);
s32 sceNpMatching2JoinRoom(SceNpMatching2CtxId ctxId,
                             SceNpMatching2RoomId roomId,
                             const void* optParam,
                             SceNpMatching2RequestId* reqId);
s32 sceNpMatching2LeaveRoom(SceNpMatching2CtxId ctxId,
                              SceNpMatching2RequestId* reqId);
s32 sceNpMatching2SearchRoom(SceNpMatching2CtxId ctxId,
                               const void* optParam,
                               SceNpMatching2RequestId* reqId);

/* Request polling */
s32 sceNpMatching2AbortRequest(SceNpMatching2CtxId ctxId,
                                 SceNpMatching2RequestId reqId);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_MATCHING2_H */

/*
 * ps3recomp - sceNpBasic HLE
 *
 * NP Basic: friends list, presence, messaging, and invitation management.
 * Games use this for online friend features and game invitations.
 */

#ifndef PS3RECOMP_SCE_NP_BASIC_H
#define PS3RECOMP_SCE_NP_BASIC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define SCE_NP_BASIC_ERROR_NOT_INITIALIZED         0x8002AA01
#define SCE_NP_BASIC_ERROR_ALREADY_INITIALIZED     0x8002AA02
#define SCE_NP_BASIC_ERROR_INVALID_ARGUMENT        0x8002AA03
#define SCE_NP_BASIC_ERROR_NOT_SUPPORTED           0x8002AA04
#define SCE_NP_BASIC_ERROR_OUT_OF_MEMORY           0x8002AA05
#define SCE_NP_BASIC_ERROR_EXCEEDS_MAX             0x8002AA06
#define SCE_NP_BASIC_ERROR_NOT_CONNECTED           0x8002AA07
#define SCE_NP_BASIC_ERROR_DATA_NOT_FOUND          0x8002AA08

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define SCE_NP_BASIC_MAX_FRIENDS           100
#define SCE_NP_BASIC_MAX_BLOCKLIST         100
#define SCE_NP_BASIC_MAX_MESSAGE_SIZE      512
#define SCE_NP_BASIC_PRESENCE_MAX_SIZE     128
#define SCE_NP_ONLINEID_MAX_LENGTH         16

/* Friend status */
#define SCE_NP_BASIC_FRIEND_STATUS_OFFLINE    0
#define SCE_NP_BASIC_FRIEND_STATUS_ONLINE     1
#define SCE_NP_BASIC_FRIEND_STATUS_AWAY       2

/* Event types for the event handler callback */
#define SCE_NP_BASIC_EVENT_INCOMING_MESSAGE   1
#define SCE_NP_BASIC_EVENT_FRIEND_ONLINE      2
#define SCE_NP_BASIC_EVENT_FRIEND_OFFLINE     3
#define SCE_NP_BASIC_EVENT_INCOMING_INVITE    4
#define SCE_NP_BASIC_EVENT_FRIEND_PRESENCE    5

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

typedef struct SceNpOnlineId {
    char data[SCE_NP_ONLINEID_MAX_LENGTH + 1];
    char padding[3];
} SceNpOnlineId;

typedef struct SceNpBasicFriendListEntry {
    SceNpOnlineId onlineId;
    u32           status;
} SceNpBasicFriendListEntry;

typedef struct SceNpBasicPresence {
    char title[SCE_NP_BASIC_PRESENCE_MAX_SIZE];
    char status[SCE_NP_BASIC_PRESENCE_MAX_SIZE];
    u8   data[64];
    u32  dataSize;
} SceNpBasicPresence;

/* Callback types */
typedef void (*SceNpBasicEventHandler)(s32 event, s32 retCode,
                                        u32 reqId, void* arg);
typedef void (*SceNpBasicPresenceHandler)(const SceNpOnlineId* onlineId,
                                           const SceNpBasicPresence* presence,
                                           void* arg);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 sceNpBasicInit(void* poolMem, u32 poolSize);
s32 sceNpBasicTerm(void);

/* Event handler */
s32 sceNpBasicRegisterHandler(SceNpBasicEventHandler handler,
                               SceNpBasicPresenceHandler presenceHandler,
                               void* arg);
s32 sceNpBasicUnregisterHandler(void);

/* Friends list */
s32 sceNpBasicGetFriendListEntryCount(u32* count);
s32 sceNpBasicGetFriendListEntry(u32 index, SceNpBasicFriendListEntry* entry);
s32 sceNpBasicGetFriendPresence(const SceNpOnlineId* onlineId,
                                 SceNpBasicPresence* presence);

/* Own presence */
s32 sceNpBasicSetPresence(const SceNpBasicPresence* presence);

/* Messaging */
s32 sceNpBasicSendMessage(const SceNpOnlineId* to,
                           const void* body, u32 bodySize);
s32 sceNpBasicSendMessageAttachment(const SceNpOnlineId* to,
                                      const char* subject,
                                      const void* data, u32 dataSize);

/* Invitation */
s32 sceNpBasicSendInGameInvitation(const SceNpOnlineId* to,
                                     const void* data, u32 dataSize);
s32 sceNpBasicRecvInGameInvitation(void* data, u32 dataMaxSize,
                                     u32* dataSize);

/* Block list */
s32 sceNpBasicGetBlockListEntryCount(u32* count);
s32 sceNpBasicAddBlockListEntry(const SceNpOnlineId* onlineId);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_BASIC_H */

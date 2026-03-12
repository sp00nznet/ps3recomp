/*
 * ps3recomp - sceNpBasic HLE implementation
 *
 * Offline stub: reports empty friends/block lists, accepts presence
 * updates silently, returns NOT_CONNECTED for messaging. Sufficient
 * for games that check NP Basic but don't require live PSN.
 */

#include "sceNpBasic.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_initialized = 0;
static SceNpBasicEventHandler s_event_handler = NULL;
static SceNpBasicPresenceHandler s_presence_handler = NULL;
static void* s_handler_arg = NULL;
static SceNpBasicPresence s_own_presence;

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 sceNpBasicInit(void* poolMem, u32 poolSize)
{
    (void)poolMem;
    (void)poolSize;
    printf("[sceNpBasic] Init(poolSize=%u)\n", poolSize);

    if (s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_ALREADY_INITIALIZED;

    memset(&s_own_presence, 0, sizeof(s_own_presence));
    s_event_handler = NULL;
    s_presence_handler = NULL;
    s_handler_arg = NULL;
    s_initialized = 1;
    return CELL_OK;
}

s32 sceNpBasicTerm(void)
{
    printf("[sceNpBasic] Term()\n");

    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    s_initialized = 0;
    s_event_handler = NULL;
    s_presence_handler = NULL;
    return CELL_OK;
}

s32 sceNpBasicRegisterHandler(SceNpBasicEventHandler handler,
                               SceNpBasicPresenceHandler presenceHandler,
                               void* arg)
{
    printf("[sceNpBasic] RegisterHandler()\n");

    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    s_event_handler = handler;
    s_presence_handler = presenceHandler;
    s_handler_arg = arg;
    return CELL_OK;
}

s32 sceNpBasicUnregisterHandler(void)
{
    printf("[sceNpBasic] UnregisterHandler()\n");

    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    s_event_handler = NULL;
    s_presence_handler = NULL;
    s_handler_arg = NULL;
    return CELL_OK;
}

s32 sceNpBasicGetFriendListEntryCount(u32* count)
{
    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    if (!count)
        return (s32)SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;

    *count = 0; /* offline -- no friends */
    return CELL_OK;
}

s32 sceNpBasicGetFriendListEntry(u32 index, SceNpBasicFriendListEntry* entry)
{
    (void)index;
    (void)entry;

    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    return (s32)SCE_NP_BASIC_ERROR_DATA_NOT_FOUND;
}

s32 sceNpBasicGetFriendPresence(const SceNpOnlineId* onlineId,
                                 SceNpBasicPresence* presence)
{
    (void)onlineId;
    (void)presence;

    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    return (s32)SCE_NP_BASIC_ERROR_DATA_NOT_FOUND;
}

s32 sceNpBasicSetPresence(const SceNpBasicPresence* presence)
{
    printf("[sceNpBasic] SetPresence()\n");

    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    if (!presence)
        return (s32)SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;

    s_own_presence = *presence;
    return CELL_OK;
}

s32 sceNpBasicSendMessage(const SceNpOnlineId* to,
                           const void* body, u32 bodySize)
{
    (void)body;
    (void)bodySize;

    printf("[sceNpBasic] SendMessage(to=%.16s)\n",
           to ? to->data : "(null)");

    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    return (s32)SCE_NP_BASIC_ERROR_NOT_CONNECTED;
}

s32 sceNpBasicSendMessageAttachment(const SceNpOnlineId* to,
                                      const char* subject,
                                      const void* data, u32 dataSize)
{
    (void)data;
    (void)dataSize;

    printf("[sceNpBasic] SendMessageAttachment(to=%.16s, subj=%s)\n",
           to ? to->data : "(null)",
           subject ? subject : "(null)");

    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    return (s32)SCE_NP_BASIC_ERROR_NOT_CONNECTED;
}

s32 sceNpBasicSendInGameInvitation(const SceNpOnlineId* to,
                                     const void* data, u32 dataSize)
{
    (void)data;
    (void)dataSize;

    printf("[sceNpBasic] SendInGameInvitation(to=%.16s)\n",
           to ? to->data : "(null)");

    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    return (s32)SCE_NP_BASIC_ERROR_NOT_CONNECTED;
}

s32 sceNpBasicRecvInGameInvitation(void* data, u32 dataMaxSize,
                                     u32* dataSize)
{
    (void)data;
    (void)dataMaxSize;

    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    if (dataSize)
        *dataSize = 0;

    return (s32)SCE_NP_BASIC_ERROR_DATA_NOT_FOUND;
}

s32 sceNpBasicGetBlockListEntryCount(u32* count)
{
    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    if (!count)
        return (s32)SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;

    *count = 0;
    return CELL_OK;
}

s32 sceNpBasicAddBlockListEntry(const SceNpOnlineId* onlineId)
{
    printf("[sceNpBasic] AddBlockListEntry(%.16s)\n",
           onlineId ? onlineId->data : "(null)");

    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    /* No-op in offline mode */
    return CELL_OK;
}

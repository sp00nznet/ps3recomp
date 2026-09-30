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
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */

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

/* The title's main loop drains PSN events with a loop that exits ONLY when
 * sceNpBasicGetEvent returns the specific code 0x8002A66A (SCE_NP_BASIC_ERROR_NO_EVENT)
 * -- verified in the recompiled drain loop func_00063E34 (cmpwi r3, 0x8002A66A ->
 * exit). Returning any OTHER error (e.g. 0x8002AA09) is NOT recognized: the game
 * then reads a STALE/garbage event out-param and loops forever, hanging the boot
 * before NP-init/trophy/GThread. So the exact value matters. We report no event
 * and don't touch the out-params. */
#ifndef SCE_NP_BASIC_ERROR_NO_EVENT
#define SCE_NP_BASIC_ERROR_NO_EVENT 0x8002A66A
#endif
s32 sceNpBasicGetEvent(u32* event, void* from, void* data, u32* size)
{
    /* out-params are GUEST EAs; the game checks the return (0x8002A66A) and
     * exits the drain BEFORE reading them, so we leave them untouched. */
    (void)event; (void)from; (void)data; (void)size;
    return (s32)SCE_NP_BASIC_ERROR_NO_EVENT;
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

/* The list counts answer without sceNpBasicInit: titles built on libsceNp2
 * never call it (NP init brings Basic up), and Simpsons Arcade asks whether a
 * joining player is on its block list -- an error there reads as "unknown". */
s32 sceNpBasicGetFriendListEntryCount(u32* count)
{
    if (!count)
        return (s32)SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)count, (u32)0); /* offline -- no friends */
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
    presence = GUEST_PTR(presence, const SceNpBasicPresence*);
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
    to = GUEST_PTR(to, const SceNpOnlineId*);
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
    to = GUEST_PTR(to, const SceNpOnlineId*);
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
    to = GUEST_PTR(to, const SceNpOnlineId*);
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
        vm_write32((u32)(uintptr_t)dataSize, (u32)0);

    return (s32)SCE_NP_BASIC_ERROR_DATA_NOT_FOUND;
}

s32 sceNpBasicGetBlockListEntryCount(u32* count)
{
    if (!count)
        return (s32)SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)count, (u32)0);
    return CELL_OK;
}

s32 sceNpBasicAddBlockListEntry(const SceNpOnlineId* onlineId)
{
    onlineId = GUEST_PTR(onlineId, const SceNpOnlineId*);
    printf("[sceNpBasic] AddBlockListEntry(%.16s)\n",
           onlineId ? onlineId->data : "(null)");

    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    /* No-op in offline mode */
    return CELL_OK;
}

/* Players met online, and the context-sensitive (XMB) handler: nothing is
 * recorded, the history is empty, the handler is accepted and never fires. */
s32 sceNpBasicGetPlayersHistoryEntryCount(u32 options, u32* count)
{
    (void)options;
    if (!count) return (s32)SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)count, 0);
    return CELL_OK;
}

s32 sceNpBasicAddPlayersHistoryAsync(const void* npids, u32 count, const void* description, u32* reqId)
{
    (void)npids; (void)count; (void)description;
    if (reqId) vm_write32((u32)(uintptr_t)reqId, 1);
    return CELL_OK;
}

s32 sceNpBasicRegisterContextSensitiveHandler(const void* context, void* handler, void* arg)
{
    (void)context; (void)handler; (void)arg;
    return CELL_OK;
}

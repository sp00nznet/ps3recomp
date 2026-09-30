/*
 * ps3recomp - cellSysutilAvc2 (in-game voice chat): accepted, silent
 *
 * No audio moves. Loading, joining and leaving the chat all succeed and say
 * so through the title's Avc2 callback, and there is never a microphone
 * attached. That is enough for a title whose online session waits on voice
 * chat to come up: Simpsons Arcade loads Avc2 as a player joins a match
 * (voice chat is on by default) and gave up on the session when the load
 * never completed.
 *
 * ponytail: voice itself would mean capturing the host microphone and
 * sending it over the room's P2P link. Add it when someone wants to talk.
 *
 * Callback: void cb(u32 event_id, u32 event_param, void* userdata), delivered
 * on the title's next cellSysutilCheckCallback.
 */

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "../system/cellSysutil.h"
#include "../../runtime/ppu/ppu_memory.h"

#include <stdio.h>
#include <string.h>

#define AVC2_EVENT_LOAD_SUCCEEDED    0x00000001u
#define AVC2_EVENT_UNLOAD_SUCCEEDED  0x00000003u
#define AVC2_EVENT_JOIN_SUCCEEDED    0x00000005u
#define AVC2_EVENT_LEAVE_SUCCEEDED   0x00000007u

static u32 s_cb, s_userdata;

static void avc2_event(u32 event)
{
    const u64 args[8] = { event, 0, s_userdata, 0, 0, 0, 0, 0 };
    if (s_cb) cellSysutilQueueGuestCallbackArgs(s_cb, args);
}

/* (version, CellSysutilAvc2InitParam* option): the title's defaults stand. */
s32 cellSysutilAvc2InitParam(u16 version, u32 option)
{
    (void)version; (void)option;
    return CELL_OK;
}

/* (ctxId, container, callback, userdata, initParam) */
s32 cellSysutilAvc2LoadAsync(u16 ctxId, u32 container, u32 callback, u32 userdata, u32 initParam)
{
    (void)ctxId; (void)container; (void)initParam;
    s_cb = callback;
    s_userdata = userdata;
    printf("[cellSysutilAvc2] LoadAsync: voice chat up, silent\n");
    avc2_event(AVC2_EVENT_LOAD_SUCCEEDED);
    return CELL_OK;
}

s32 cellSysutilAvc2UnloadAsync(void)
{
    avc2_event(AVC2_EVENT_UNLOAD_SUCCEEDED);
    return CELL_OK;
}

/* (const SceNpMatching2RoomId* roomId) */
s32 cellSysutilAvc2JoinChatRequest(u32 roomId)
{
    (void)roomId;
    avc2_event(AVC2_EVENT_JOIN_SUCCEEDED);
    return CELL_OK;
}

s32 cellSysutilAvc2LeaveChatRequest(void)
{
    avc2_event(AVC2_EVENT_LEAVE_SUCCEEDED);
    return CELL_OK;
}

s32 cellSysutilAvc2StartStreaming(void) { return CELL_OK; }
s32 cellSysutilAvc2StopStreaming(void)  { return CELL_OK; }

s32 cellSysutilAvc2EnableVoiceDetection(u8 enable, u8 level) { (void)enable; (void)level; return CELL_OK; }

/* (u8 device_type, u8* status): no microphone. */
s32 cellSysutilAvc2IsMicAttached(u8 deviceType, u32 status)
{
    (void)deviceType;
    if (status) vm_write8(status, 0);
    return CELL_OK;
}

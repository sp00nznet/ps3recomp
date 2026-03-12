/*
 * ps3recomp - sceNpClans HLE implementation
 *
 * Offline stub. Init/term work, all clan operations return NOT_CONNECTED.
 */

#include "sceNpClans.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

static int s_initialized = 0;

/* API */

s32 sceNpClansInit(const void* commId, const void* passPhrase, u32 poolSize)
{
    (void)commId; (void)passPhrase; (void)poolSize;
    printf("[sceNpClans] Init()\n");
    if (s_initialized)
        return (s32)SCE_NP_CLANS_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 sceNpClansTerm(void)
{
    printf("[sceNpClans] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 sceNpClansCreateClan(const char* name, const char* tag,
                           SceNpClansRequestId* reqId)
{
    (void)name; (void)tag; (void)reqId;
    printf("[sceNpClans] CreateClan() - offline\n");
    return (s32)SCE_NP_CLANS_ERROR_NOT_CONNECTED;
}

s32 sceNpClansLeaveClan(SceNpClansClanId clanId, SceNpClansRequestId* reqId)
{
    (void)clanId; (void)reqId;
    return (s32)SCE_NP_CLANS_ERROR_NOT_CONNECTED;
}

s32 sceNpClansJoinClan(SceNpClansClanId clanId, SceNpClansRequestId* reqId)
{
    (void)clanId; (void)reqId;
    return (s32)SCE_NP_CLANS_ERROR_NOT_CONNECTED;
}

s32 sceNpClansGetClanInfo(SceNpClansClanId clanId, SceNpClansClanInfo* info,
                            SceNpClansRequestId* reqId)
{
    (void)clanId; (void)info; (void)reqId;
    return (s32)SCE_NP_CLANS_ERROR_NOT_CONNECTED;
}

s32 sceNpClansSearchByName(const char* name, SceNpClansRequestId* reqId)
{
    (void)name; (void)reqId;
    return (s32)SCE_NP_CLANS_ERROR_NOT_CONNECTED;
}

s32 sceNpClansGetMemberList(SceNpClansClanId clanId, SceNpClansRequestId* reqId)
{
    (void)clanId; (void)reqId;
    return (s32)SCE_NP_CLANS_ERROR_NOT_CONNECTED;
}

s32 sceNpClansAbortRequest(SceNpClansRequestId reqId)
{
    (void)reqId;
    return CELL_OK;
}

s32 sceNpClansCheckCallback(void)
{
    if (!s_initialized)
        return (s32)SCE_NP_CLANS_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

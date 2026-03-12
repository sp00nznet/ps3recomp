/*
 * ps3recomp - sceNpSns HLE implementation
 *
 * Offline stub. Init/term work, social operations return NOT_CONNECTED.
 */

#include "sceNpSns.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

static int s_initialized = 0;

/* API */

s32 sceNpSnsInit(u32 poolSize)
{
    (void)poolSize;
    printf("[sceNpSns] Init()\n");
    if (s_initialized)
        return (s32)SCE_NP_SNS_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 sceNpSnsTerm(void)
{
    printf("[sceNpSns] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 sceNpSnsFbCheckThisUserAccount(SceNpSnsAccountId* accountId)
{
    (void)accountId;
    printf("[sceNpSns] FbCheckThisUserAccount() - offline\n");
    return (s32)SCE_NP_SNS_ERROR_NOT_CONNECTED;
}

s32 sceNpSnsFbGetLongAccessToken(char* token, u32 tokenSize)
{
    (void)token; (void)tokenSize;
    return (s32)SCE_NP_SNS_ERROR_NOT_CONNECTED;
}

s32 sceNpSnsFbPostFeedMessage(const SceNpSnsFbFeedMessage* message)
{
    (void)message;
    printf("[sceNpSns] FbPostFeedMessage() - offline\n");
    return (s32)SCE_NP_SNS_ERROR_NOT_CONNECTED;
}

s32 sceNpSnsCheckCallback(void)
{
    if (!s_initialized)
        return (s32)SCE_NP_SNS_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

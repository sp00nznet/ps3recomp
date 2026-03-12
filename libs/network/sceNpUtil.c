/*
 * ps3recomp - sceNpUtil HLE implementation
 *
 * Reports production environment, validates NP IDs, and provides
 * no-restriction parental controls. Bandwidth test reports a fast
 * fake connection.
 */

#include "sceNpUtil.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static s32 s_np_env = SCE_NP_ENV_PRODUCTION;
static int s_bw_test_running = 0;

/* ---------------------------------------------------------------------------
 * Bandwidth test
 * -----------------------------------------------------------------------*/

s32 sceNpUtilBandwidthTestInitStart(u32 flags)
{
    (void)flags;
    printf("[sceNpUtil] BandwidthTestInitStart()\n");
    s_bw_test_running = 1;
    return CELL_OK;
}

s32 sceNpUtilBandwidthTestGetStatus(SceNpBandwidthTestResult* result)
{
    if (!result)
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;

    /* Report a generous fake bandwidth (100 Mbps) */
    result->uploadBps = 100000000.0;
    result->downloadBps = 100000000.0;
    result->result = 0; /* success / done */

    s_bw_test_running = 0;
    return CELL_OK;
}

s32 sceNpUtilBandwidthTestShutdown(void)
{
    printf("[sceNpUtil] BandwidthTestShutdown()\n");
    s_bw_test_running = 0;
    return CELL_OK;
}

s32 sceNpUtilBandwidthTestAbort(void)
{
    printf("[sceNpUtil] BandwidthTestAbort()\n");
    s_bw_test_running = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP environment
 * -----------------------------------------------------------------------*/

s32 sceNpUtilGetNpEnv(s32* env)
{
    if (!env)
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;

    *env = s_np_env;
    return CELL_OK;
}

s32 sceNpUtilSetNpEnv(s32 env)
{
    printf("[sceNpUtil] SetNpEnv(%d)\n", env);
    s_np_env = env;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP ID validation
 * -----------------------------------------------------------------------*/

s32 sceNpUtilCheckOnlineId(const char* onlineId)
{
    if (!onlineId)
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;

    /* PS3 online IDs: 3-16 chars, alphanumeric + dash + underscore,
       must start with a letter */
    size_t len = strlen(onlineId);
    if (len < 3 || len > 16)
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ONLINE_ID;

    if (!isalpha((unsigned char)onlineId[0]))
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ONLINE_ID;

    for (size_t i = 0; i < len; i++) {
        char c = onlineId[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_')
            return (s32)SCE_NP_UTIL_ERROR_INVALID_ONLINE_ID;
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Parental control
 * -----------------------------------------------------------------------*/

s32 sceNpUtilGetParentalControlInfo(s32* age, s32* chatRestriction)
{
    if (age)
        *age = 18; /* no restriction */
    if (chatRestriction)
        *chatRestriction = 0; /* chat allowed */
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP ID comparison
 * -----------------------------------------------------------------------*/

s32 sceNpUtilCmpNpId(const void* npId1, const void* npId2)
{
    if (!npId1 || !npId2)
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;

    return memcmp(npId1, npId2, 36) == 0 ? 0 : 1;
}

s32 sceNpUtilCmpNpIdInOrder(const void* npId1, const void* npId2, s32* order)
{
    if (!npId1 || !npId2 || !order)
        return (s32)SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;

    *order = memcmp(npId1, npId2, 36);
    return CELL_OK;
}

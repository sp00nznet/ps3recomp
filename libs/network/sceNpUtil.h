/*
 * ps3recomp - sceNpUtil HLE
 *
 * NP utility functions: bandwidth test, NP environment queries,
 * parental control, NP ID validation.
 */

#ifndef PS3RECOMP_SCE_NP_UTIL_H
#define PS3RECOMP_SCE_NP_UTIL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define SCE_NP_UTIL_ERROR_NOT_INITIALIZED      0x80022501
#define SCE_NP_UTIL_ERROR_INVALID_ARGUMENT     0x80022502
#define SCE_NP_UTIL_ERROR_OUT_OF_MEMORY        0x80022503
#define SCE_NP_UTIL_ERROR_INVALID_NP_ENV       0x80022504
#define SCE_NP_UTIL_ERROR_INVALID_ONLINE_ID    0x80022505

/* ---------------------------------------------------------------------------
 * NP Environment
 * -----------------------------------------------------------------------*/
#define SCE_NP_ENV_PRODUCTION      0
#define SCE_NP_ENV_QA              1
#define SCE_NP_ENV_SP_INT          2

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

typedef struct SceNpBandwidthTestResult {
    double uploadBps;
    double downloadBps;
    s32    result;
} SceNpBandwidthTestResult;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* Bandwidth test */
s32 sceNpUtilBandwidthTestInitStart(u32 flags);
s32 sceNpUtilBandwidthTestGetStatus(SceNpBandwidthTestResult* result);
s32 sceNpUtilBandwidthTestShutdown(void);
s32 sceNpUtilBandwidthTestAbort(void);

/* NP environment */
s32 sceNpUtilGetNpEnv(s32* env);
s32 sceNpUtilSetNpEnv(s32 env);

/* NP ID validation */
s32 sceNpUtilCheckOnlineId(const char* onlineId);

/* Parental control */
s32 sceNpUtilGetParentalControlInfo(s32* age, s32* chatRestriction);

/* CRC */
s32 sceNpUtilCmpNpId(const void* npId1, const void* npId2);
s32 sceNpUtilCmpNpIdInOrder(const void* npId1, const void* npId2, s32* order);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_UTIL_H */

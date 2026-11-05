/*
 * ps3recomp - cellSysutilNpEula HLE
 *
 * The PSN terms-of-service (EULA) check a title runs before its online
 * features. The answer always arrives through the title's callback on its
 * next cellSysutilCheckCallback, as on the console; cellSysutilNpEula.c says
 * which answer.
 */

#ifndef PS3RECOMP_CELL_SYSUTIL_NP_EULA_H
#define PS3RECOMP_CELL_SYSUTIL_NP_EULA_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SceNpEulaStatus, the callback's first argument (RPCS3 cellSysutilNpEula.cpp). */
#define SCE_NP_EULA_UNKNOWN           0
#define SCE_NP_EULA_ACCEPTED          1
#define SCE_NP_EULA_ALREADY_ACCEPTED  2
#define SCE_NP_EULA_REJECTED          3
#define SCE_NP_EULA_ABORTED           4
#define SCE_NP_EULA_ERROR             5

#define SCE_NP_EULA_ERROR_UNKNOWN             0x8002e500
#define SCE_NP_EULA_ERROR_INVALID_ARGUMENT    0x8002e501
#define SCE_NP_EULA_ERROR_NOT_INITIALIZED     0x8002e502
#define SCE_NP_EULA_ERROR_ALREADY_INITIALIZED 0x8002e503

/* Pointers (communicationId, cbFunc = an OPD, cbFuncArg) are guest addresses:
 * the HLE ABI passes them through untranslated.
 *   check: cbFunc(status, errorCode, version, cbFuncArg)
 *   show:  cbFunc(status, param, cbFuncArg), a CellSysutilCallback          */
s32 sceNpEulaCheckEulaStatus(u32 communicationId, u32 arg2, u64 arg3,
                             u32 cbFunc, u32 cbFuncArg);              /* NID 0x59D1629A */
s32 sceNpEulaAbort(void);                                             /* NID 0x5EC05AD8 */
s32 sceNpEulaShowCurrentEula(u32 communicationId, u64 arg2,
                             u32 cbFunc, u32 cbFuncArg);              /* NID 0x6599500D */

/* Called by cellSysutilCheckCallback: moves a pending answer to the guest
 * callback queue, which that same poll drains (RPCS3's sysutil_register_cb). */
void ps3_np_eula_tick(void);

#ifdef __cplusplus
}
#endif
#endif

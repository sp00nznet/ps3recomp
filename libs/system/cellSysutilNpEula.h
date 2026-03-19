/*
 * ps3recomp - cellSysutilNpEula HLE
 *
 * NP EULA (End User License Agreement) acceptance dialog.
 * Returns "already accepted" so games proceed past the EULA check.
 */

#ifndef PS3RECOMP_CELL_SYSUTIL_NP_EULA_H
#define PS3RECOMP_CELL_SYSUTIL_NP_EULA_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

s32 cellSysutilNpEulaCheck(void* cb, void* arg);

#ifdef __cplusplus
}
#endif
#endif

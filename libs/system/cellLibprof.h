/*
 * ps3recomp - cellLibprof (libprof) HLE
 *
 * The user-trace registration pair firmware libsre imports. libprof is the
 * Performance Analyzer's library; with no analyzer attached there is nothing
 * to register with.
 */

#ifndef PS3RECOMP_CELL_LIBPROF_H
#define PS3RECOMP_CELL_LIBPROF_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

s32 cellUserTraceRegister(void);
s32 cellUserTraceUnregister(void);

#ifdef __cplusplus
}
#endif

#endif

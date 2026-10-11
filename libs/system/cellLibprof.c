/*
 * ps3recomp - cellLibprof (libprof) HLE implementation
 *
 * Both return CELL_OK, as in RPCS3 (tests/conformance/spurs/t_sysprx). That
 * comparison is weaker than the rest of the suite: RPCS3 does not run Sony's
 * libprof but its own HLE, which is these same two stubs, so it confirms the
 * return code libsre sees there and nothing about the real library.
 */

#include "cellLibprof.h"

s32 cellUserTraceRegister(void)
{
    return CELL_OK;
}

s32 cellUserTraceUnregister(void)
{
    return CELL_OK;
}

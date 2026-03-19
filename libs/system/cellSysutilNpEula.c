/*
 * ps3recomp - cellSysutilNpEula HLE implementation
 *
 * EULA check: returns "already accepted" so games skip the EULA dialog.
 */

#include "cellSysutilNpEula.h"
#include <stdio.h>

s32 cellSysutilNpEulaCheck(void* cb, void* arg)
{
    (void)cb; (void)arg;
    printf("[cellSysutilNpEula] EulaCheck() — returning accepted\n");
    /* Return 1 = EULA already accepted */
    return 1;
}

/*
 * ps3recomp - cellLicenseArea HLE implementation
 *
 * Stub. Reports Americas region, all areas valid.
 */

#include "cellLicenseArea.h"
#include <stdio.h>

/* API */

s32 cellLicenseAreaCheck(s32* areaCode)
{
    printf("[cellLicenseArea] Check()\n");
    if (!areaCode) return (s32)CELL_LICENSE_AREA_ERROR_INVALID_ARGUMENT;
    *areaCode = CELL_LICENSE_AREA_A; /* Americas */
    return CELL_OK;
}

s32 cellLicenseAreaGetAreaCode(s32* areaCode)
{
    if (!areaCode) return (s32)CELL_LICENSE_AREA_ERROR_INVALID_ARGUMENT;
    *areaCode = CELL_LICENSE_AREA_A;
    return CELL_OK;
}

s32 cellLicenseAreaIsValid(s32 areaCode, s32* isValid)
{
    (void)areaCode;
    if (!isValid) return (s32)CELL_LICENSE_AREA_ERROR_INVALID_ARGUMENT;
    *isValid = 1; /* always valid */
    return CELL_OK;
}

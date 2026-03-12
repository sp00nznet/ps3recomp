/*
 * ps3recomp - cellLicenseArea HLE
 *
 * License area verification for region-locked content.
 * Stub — always reports valid license, no restrictions.
 */

#ifndef PS3RECOMP_CELL_LICENSE_AREA_H
#define PS3RECOMP_CELL_LICENSE_AREA_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_LICENSE_AREA_ERROR_INVALID_ARGUMENT  0x80410B01
#define CELL_LICENSE_AREA_ERROR_NOT_FOUND         0x80410B02

/* License area IDs */
#define CELL_LICENSE_AREA_J    1   /* Japan */
#define CELL_LICENSE_AREA_A    2   /* Americas */
#define CELL_LICENSE_AREA_E    3   /* Europe */
#define CELL_LICENSE_AREA_H    4   /* Asia (Hong Kong) */
#define CELL_LICENSE_AREA_K    5   /* Korea */
#define CELL_LICENSE_AREA_C    6   /* China */

/* Functions */
s32 cellLicenseAreaCheck(s32* areaCode);
s32 cellLicenseAreaGetAreaCode(s32* areaCode);
s32 cellLicenseAreaIsValid(s32 areaCode, s32* isValid);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_LICENSE_AREA_H */

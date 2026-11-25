/*
 * ps3recomp - cellPrint HLE
 *
 * Print utility — PS3 printing support for compatible USB printers.
 * Stub — init/end lifecycle, print operations return NOT_SUPPORTED.
 */

#ifndef PS3RECOMP_CELL_PRINT_H
#define PS3RECOMP_CELL_PRINT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (sysutil/sysutil_print.h; base 0x8002C400 verified against Sony
 * firmware 4.93 libsysutil_print.sprx: GetStatus(0)/StartJob(0)/SendBand(0) -> 0x8002C404) */
#define CELL_PRINT_ERROR_NOT_INITIALIZED     (CELL_ERROR_BASE_SYSUTIL_PRINT | 0x01) /* 0x8002C401 */
#define CELL_PRINT_ERROR_ALREADY_INITIALIZED (CELL_ERROR_BASE_SYSUTIL_PRINT | 0x02) /* 0x8002C402 */
#define CELL_PRINT_ERROR_NOT_SUPPORTED       (CELL_ERROR_BASE_SYSUTIL_PRINT | 0x03) /* 0x8002C403 */
#define CELL_PRINT_ERROR_INVALID_PARAM       (CELL_ERROR_BASE_SYSUTIL_PRINT | 0x04) /* 0x8002C404 */
#define CELL_PRINT_ERROR_INVALID_ARGUMENT    CELL_PRINT_ERROR_INVALID_PARAM
#define CELL_PRINT_ERROR_NO_PRINTER          (CELL_ERROR_BASE_SYSUTIL_PRINT | 0x05) /* 0x8002C405 */

/* Functions */
s32 cellPrintInit(void);
s32 cellPrintEnd(void);
s32 cellPrintGetPrinterCount(u32* count);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_PRINT_H */

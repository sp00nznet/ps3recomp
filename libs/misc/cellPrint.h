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

/* Error codes */
#define CELL_PRINT_ERROR_NOT_INITIALIZED     0x80610A01
#define CELL_PRINT_ERROR_ALREADY_INITIALIZED 0x80610A02
#define CELL_PRINT_ERROR_INVALID_ARGUMENT    0x80610A03
#define CELL_PRINT_ERROR_NOT_SUPPORTED       0x80610A04
#define CELL_PRINT_ERROR_NO_PRINTER          0x80610A05

/* Functions */
s32 cellPrintInit(void);
s32 cellPrintEnd(void);
s32 cellPrintGetPrinterCount(u32* count);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_PRINT_H */

/*
 * ps3recomp - cellPrint HLE implementation
 *
 * Stub. Init/term work, no printers detected.
 */

#include "cellPrint.h"
#include <stdio.h>

/* Internal state */

static int s_initialized = 0;

/* API */

s32 cellPrintInit(void)
{
    printf("[cellPrint] Init()\n");
    if (s_initialized)
        return (s32)CELL_PRINT_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellPrintEnd(void)
{
    printf("[cellPrint] End()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellPrintGetPrinterCount(u32* count)
{
    if (!s_initialized)
        return (s32)CELL_PRINT_ERROR_NOT_INITIALIZED;
    if (!count) return (s32)CELL_PRINT_ERROR_INVALID_ARGUMENT;
    *count = 0; /* No printers */
    return CELL_OK;
}

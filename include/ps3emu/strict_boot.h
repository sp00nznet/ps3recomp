/* Shared C/C++ diagnostic stop. Opt in before launch with PS3_STRICT_BOOT=1.
 * This guard reports unsupported execution; it does not implement it. */
#ifndef PS3EMU_STRICT_BOOT_H
#define PS3EMU_STRICT_BOOT_H
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int ps3_strict_boot_enabled(void)
{
    const char* value = getenv("PS3_STRICT_BOOT");
    return value && strcmp(value, "1") == 0;
}

static inline void ps3_strict_boot_guard(const char* category,
                                         const char* format, ...)
{
    va_list args;
    if (!ps3_strict_boot_enabled()) return;
    fprintf(stderr, "[strict-boot] FATAL category=%s ", category);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, " exit=86\n");
    fflush(NULL);
    _Exit(86);
}
#endif

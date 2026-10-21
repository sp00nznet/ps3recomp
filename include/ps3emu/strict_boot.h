/* Shared C/C++ diagnostic stop. Opt in before launch with PS3_STRICT_BOOT=1.
 * This guard reports unsupported execution; it does not implement it. */
#ifndef PS3EMU_STRICT_BOOT_H
#define PS3EMU_STRICT_BOOT_H
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

static inline int ps3_strict_boot_enabled(void)
{
    const char* value = getenv("PS3_STRICT_BOOT");
    return value && strcmp(value, "1") == 0;
}

/* For per-operation SPU paths (every DMA, channel read and FP status check).
 * getenv takes the CRT environment lock and scans every variable; there it
 * cost more than Folklore's audio job itself and starved the audio ring.
 * Strict mode is opted into before launch and never revoked inside a process,
 * so once seen it is remembered; until then the environment is read each time. */
static inline int ps3_strict_boot_latched(void)
{
    static volatile int s_seen;
    if (s_seen) return 1;
    if (!ps3_strict_boot_enabled()) return 0;
    s_seen = 1;
    return 1;
}

static inline void ps3_strict_boot_guard(const char* category,
                                         const char* format, ...)
{
    va_list args;
    if (!ps3_strict_boot_enabled()) return;
    char record[4096];
    int prefix = snprintf(record, sizeof(record), "\n[strict-boot] FATAL category=%s ", category);
    size_t used = prefix > 0 && prefix < (int)sizeof(record) ? (size_t)prefix : 0;
    va_start(args, format);
    int message = vsnprintf(record + used, sizeof(record) - used, format, args);
    va_end(args);
    if (message > 0) used += (size_t)message < sizeof(record) - used ? (size_t)message : sizeof(record) - used - 1;
    /* Reserve the fatal terminator even when a path or message is oversized. */
    static const char end[] = " exit=86\n";
    if (used > sizeof(record) - sizeof(end)) used = sizeof(record) - sizeof(end);
    memcpy(record + used, end, sizeof(end) - 1);
    used += sizeof(end) - 1;
    fflush(NULL);
    /* One OS write prevents other guest threads' stdout from splitting the
     * category and its values into separate log fragments. */
#ifdef _WIN32
    (void)_write(_fileno(stderr), record, (unsigned)used);
#else
    (void)write(STDERR_FILENO, record, used);
#endif
    _Exit(86);
}
#endif

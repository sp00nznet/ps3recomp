/* ps3_log.h -- gate for high-volume diagnostic logging.
 *
 * Chatty per-wait / per-round logs stay FULL when stderr is redirected
 * (debug runs capture to a file) or PS3_VERBOSE is set, and go quiet on a
 * live console: conhost flushes stall the emitting threads, and the ~20k
 * [WAIT] lines/minute of an LBP intro run showed up as harsh ~1 Hz hitches
 * in user-visible runs while file-redirected runs played smoothly.
 */
#ifndef PS3_LOG_H
#define PS3_LOG_H

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <io.h>
#define PS3_ISATTY_STDERR() _isatty(_fileno(stderr))
#else
#include <unistd.h>
#define PS3_ISATTY_STDERR() isatty(fileno(stderr))
#endif

#ifdef __cplusplus
static inline int ps3_log_verbose(void)
#else
static __inline int ps3_log_verbose(void)
#endif
{
    static int v = -1;
    if (v < 0) v = (getenv("PS3_VERBOSE") || !PS3_ISATTY_STDERR()) ? 1 : 0;
    return v;
}

#endif /* PS3_LOG_H */

/*
 * ps3recomp - buffered host stdout/stderr, flushed on a timer and on the way out
 *
 * The runtime logs a lot: HLE calls, syscalls, SPU dispatch, from every guest
 * thread. That output used to go through an unbuffered stdout (ppu_run set
 * _IONBF), so that a run killed from outside still left every line it printed
 * -- "the log line is absent" has to be usable as evidence.
 *
 * Unbuffered is expensive where it matters most. mingw-w64's printf
 * (__mingw_pformat) hands an unbuffered FILE one character at a time, so each
 * character becomes its own write(); under Wine into a pipe that is
 * microseconds per character, and a title that logs a thousand lines a second
 * spends most of its logging threads' time there. Lines from two threads also
 * interleave character by character.
 *
 * ps3_stdio_init() keeps the evidence and drops the cost: stdout and stderr
 * get a 64 KB buffer each (full buffering), and a small thread flushes both
 * every PS3_STDIO_FLUSH_MS milliseconds (default 50). They are also flushed at
 * exit (atexit), on abort() (SIGABRT), and when an exception reaches the
 * unhandled-exception filter (a crash; on POSIX through win32_compat's
 * dispatcher). A run killed with no chance to clean up loses at most the last
 * flush interval.
 *
 * PS3_STDIO_UNBUFFERED=1 keeps the old behaviour: stdout unbuffered, stderr
 * left as the C library set it, no thread. Use it when the exact order of
 * stdout and stderr lines in one merged stream matters: buffered, each
 * stream's lines stay in order, but within one flush interval a stdout line
 * and a stderr line can come out in either order.
 *
 * ppu_run() calls ps3_stdio_init(). A host may call it earlier itself, to
 * cover its own startup output; later calls do nothing.
 */
#ifndef PS3_STDIO_H
#define PS3_STDIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Buffer stdout and stderr and start the flush thread (see above). Idempotent.
 * Falls back to the old behaviour (stdout unbuffered) if the thread cannot be
 * started. */
void ps3_stdio_init(void);

/* Flush stdout and stderr now, from the calling thread. For normal paths. */
void ps3_stdio_flush(void);

/* Flush from a crash or abort path: asks the flush thread to flush and waits
 * for it, at most about a second. The calling thread may have died holding a
 * stream's lock (a fault inside printf), and fflush from that thread would
 * then never return; the flush thread may block on that lock instead, and the
 * caller carries on after the timeout. A host with its own crash handler that
 * ends the process (ExitProcess, _exit) calls this first. */
void ps3_stdio_flush_crash(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3_STDIO_H */

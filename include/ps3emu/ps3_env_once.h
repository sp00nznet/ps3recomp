/*
 * ps3recomp - getenv read once per call site
 *
 * Diagnostic switches are tested with getenv on hot paths (every cellFsRead,
 * every HLE SPURS job, every WAIT_SIGNAL). The Windows CRT takes its
 * environment lock in getenv, so those tests cost a lock round trip each and
 * serialize the threads making them (upstream PR #437 measured it). The
 * environment does not change while a title runs, so a file that includes
 * this header -- after <stdlib.h> -- has each `getenv("LITERAL")` looked up
 * once and kept in a static at that call site.
 *
 * Only for files whose getenv arguments are all string literals: the cache
 * is per call site, not per name. Never include it from a header.
 */
#ifndef PS3EMU_PS3_ENV_ONCE_H
#define PS3EMU_PS3_ENV_ONCE_H

#include <stdlib.h>

#if defined(__GNUC__) || defined(__clang__)
#define getenv(name) (__extension__({                                          \
    static const char* ps3__env_v;                                             \
    static int ps3__env_done;                                                  \
    if (!__atomic_load_n(&ps3__env_done, __ATOMIC_ACQUIRE)) {                  \
        ps3__env_v = (getenv)(name);                                           \
        __atomic_store_n(&ps3__env_done, 1, __ATOMIC_RELEASE);                 \
    }                                                                          \
    ps3__env_v; }))
#endif

#endif /* PS3EMU_PS3_ENV_ONCE_H */

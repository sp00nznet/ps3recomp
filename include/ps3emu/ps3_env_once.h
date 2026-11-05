/*
 * ps3recomp - ps3_env_once(): an environment switch looked up once per name
 *
 * Diagnostic switches are tested with getenv on hot paths (every cellFsRead,
 * every HLE SPURS job, every SPU workload dispatch). The Windows CRT takes its
 * environment lock in getenv, so each test is a lock round trip and the
 * threads making them serialize (upstream PR #437 measured it); elsewhere
 * getenv walks the whole environment each time.
 *
 * ps3_env_once("NAME") returns what getenv("NAME") returned the first time
 * this translation unit asked for NAME, and never calls getenv for it again.
 * The environment does not change while a title runs. Use it only for
 * switches read that way: a variable a program sets at run time and expects
 * the next read to see must stay a plain getenv.
 *
 * It is an ordinary function, so it does the same thing under every compiler
 * (MSVC, clang-cl, gcc, clang), leaves getenv itself alone, and caches only
 * where it is called. The cache is keyed by the name's text, not by the call
 * site, so any argument works, string literal or not. One table per
 * translation unit (a static inline function's static): two files that read
 * the same name look it up once each. A name of PS3_ENV_ONCE_NAME_MAX
 * characters or more, or one arriving after the table is full, goes to
 * getenv every time -- still correct, just not cached.
 */
#ifndef PS3EMU_PS3_ENV_ONCE_H
#define PS3EMU_PS3_ENV_ONCE_H

#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define PS3_ENV_ONCE_INLINE static __inline   /* MSVC's C mode before /std:c11 */
#else
#define PS3_ENV_ONCE_INLINE static inline
#endif

#define PS3_ENV_ONCE_SLOTS    64   /* a power of two */
#define PS3_ENV_ONCE_NAME_MAX 48

typedef struct {
    volatile long state;           /* 0 empty, 1 being filled, 2 ready */
    unsigned hash;
    char name[PS3_ENV_ONCE_NAME_MAX];
    const char* value;
} ps3_env_once_slot;

#if defined(_MSC_VER) && !defined(__clang__)
static __forceinline long ps3_env_once_load(volatile long* p)
{
#if defined(_M_ARM64) || defined(_M_ARM)
    return _InterlockedOr(p, 0);           /* full barrier */
#else
    const long v = *p;                     /* x86/x64 loads are acquire */
    _ReadWriteBarrier();
    return v;
#endif
}
static __forceinline void ps3_env_once_publish(volatile long* p, long v)
{
    _InterlockedExchange(p, v);            /* full barrier */
}
static __forceinline int ps3_env_once_claim(volatile long* p)
{
    return _InterlockedCompareExchange(p, 1, 0) == 0;
}
#else
PS3_ENV_ONCE_INLINE long ps3_env_once_load(volatile long* p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
PS3_ENV_ONCE_INLINE void ps3_env_once_publish(volatile long* p, long v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
PS3_ENV_ONCE_INLINE int ps3_env_once_claim(volatile long* p)
{
    long expected = 0;
    return __atomic_compare_exchange_n(p, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#endif

PS3_ENV_ONCE_INLINE const char* ps3_env_once(const char* name)
{
    static ps3_env_once_slot slots[PS3_ENV_ONCE_SLOTS];
    if (!name) return NULL;
    unsigned hash = 2166136261u;
    size_t len = 0;
    for (const char* c = name; *c; c++, len++) hash = (hash ^ (unsigned char)*c) * 16777619u;
    if (len >= PS3_ENV_ONCE_NAME_MAX) return getenv(name);
    for (unsigned i = 0; i < PS3_ENV_ONCE_SLOTS; i++) {
        ps3_env_once_slot* s = &slots[(hash + i) & (PS3_ENV_ONCE_SLOTS - 1u)];
        long state = ps3_env_once_load(&s->state);
        if (state == 0) {
            if (ps3_env_once_claim(&s->state)) {
                s->hash = hash;
                memcpy(s->name, name, len + 1);
                s->value = getenv(name);
                ps3_env_once_publish(&s->state, 2);
                return s->value;
            }
            state = ps3_env_once_load(&s->state);
        }
        /* Another thread is still filling this slot, for this name or
         * another: read the variable directly this once. */
        if (state != 2) return getenv(name);
        if (s->hash == hash && memcmp(s->name, name, len + 1) == 0) return s->value;
    }
    return getenv(name);                   /* the table is full */
}

#endif /* PS3EMU_PS3_ENV_ONCE_H */

/*
 * ps3recomp - Win32 sync/timing/type compatibility shim for POSIX hosts
 *
 * A good deal of this tree was written directly against Win32: the
 * synchronisation and timing API (SRWLOCK, CONDITION_VARIABLE, QPC, events),
 * the interlocked intrinsics behind the PPU reservation spinlock, and the
 * scalar typedefs (DWORD, ULONGLONG, SIZE_T, ...) those call sites use. This
 * header supplies all of it on POSIX so the call sites compile unchanged. On
 * Windows it is a no-op passthrough to <windows.h>.
 *
 * Usable from C AND C++. That is deliberate: the runtime library is C but the
 * PPU boot scaffold is C++, and both need these names. Nothing in here may use
 * a C-only construct -- see the note on ps3_lazy_obj below for the one that
 * did, and how far it got before anyone noticed.
 *
 * Two kinds of definition live here:
 *
 *   - static inline functions with no shared state (types, interlocked ops,
 *     SRW locks, condition variables, critical sections, timing). These need
 *     nothing linked.
 *   - functions that need state shared across translation units (waitable
 *     handles -- events, semaphores, threads, timers -- WaitOnAddress, and the
 *     VirtualAlloc region table). Those are declared here and implemented in
 *     win32_compat.c, which the runtime library compiles on every POSIX host.
 *     A standalone tool that calls one of them links that file too.
 *
 * IMPORTANT -- storage size. Win32 SRWLOCK and CONDITION_VARIABLE are exactly
 * pointer-sized and zero-initialise validly, so several structs (notably
 * spu_context::ch_wait_lock / ch_wait_cv) store them inline in a `void*` slot.
 * pthread_rwlock_t (200 B on Darwin) and pthread_cond_t (48 B) do NOT fit
 * there; casting the slot and writing a pthread object through it would
 * overflow into adjacent members. So on POSIX these map to `void*` and the
 * real object is heap-allocated on first use and published into the slot with
 * an atomic CAS. NULL still means "uninitialised", so existing memset-based
 * zero-init remains correct.
 *
 * IMPORTANT -- LONG is 32 bits here, as on Windows. MSVC's `long` is 32-bit and
 * every Win32 declaration of LONG, ULONG and the interlocked targets means
 * exactly that width. A POSIX LP64 `long` is 64 bits, and mapping LONG onto it
 * makes `InterlockedExchange((volatile LONG*)&some_u32, ...)` an 8-byte RMW on
 * a 4-byte object -- silent corruption of whatever sits next to it, on the
 * platform where it is hardest to find. So LONG is int32_t, and a call site
 * that spelt its target `volatile long` fails to compile instead: change it to
 * `volatile LONG`, which is what it always meant.
 */
#ifndef PS3RECOMP_WIN32_COMPAT_H
#define PS3RECOMP_WIN32_COMPAT_H

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else

#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <wchar.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Scalar types and constants
 * -----------------------------------------------------------------------*/
#ifndef FALSE
#  define FALSE 0
#endif
#ifndef TRUE
#  define TRUE 1
#endif
#ifndef INFINITE
#  define INFINITE 0xFFFFFFFFu
#endif

#ifndef WINAPI
#  define WINAPI
#endif
#ifndef CALLBACK
#  define CALLBACK
#endif
#ifndef APIENTRY
#  define APIENTRY
#endif

typedef int                BOOL;
typedef unsigned char      BYTE;
typedef unsigned char      UCHAR;
typedef char               CHAR;
typedef unsigned short     WORD;
#ifndef PS3_WIN32_TYPES_USHORT
#define PS3_WIN32_TYPES_USHORT
typedef unsigned short     USHORT;
#endif
typedef short              SHORT;
typedef uint32_t           DWORD;
typedef uint32_t           ULONG;
typedef uint32_t           UINT;
typedef int32_t            LONG;      /* 32-bit, as on Windows -- see the header comment */
typedef int32_t            INT;
typedef long long          LONGLONG;  /* long long, not int64_t: distinct from `long` everywhere */
typedef long long          LONG64;
typedef unsigned long long ULONGLONG;
typedef unsigned long long ULONG64;
typedef unsigned long long DWORD64;
typedef unsigned long long DWORDLONG;
typedef size_t             SIZE_T;
typedef size_t             ULONG_PTR;
typedef size_t             UINT_PTR;
typedef size_t             DWORD_PTR;
typedef intptr_t           LONG_PTR;
typedef intptr_t           SSIZE_T;
typedef void               VOID;
typedef void*              PVOID;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef void*              HANDLE;
typedef HANDLE*            PHANDLE;
typedef char*              LPSTR;
typedef char*              PSTR;
typedef const char*        LPCSTR;
typedef const char*        PCSTR;
typedef wchar_t*           LPWSTR;
typedef wchar_t*           PWSTR;
typedef const wchar_t*     LPCWSTR;
typedef const wchar_t*     PCWSTR;
typedef unsigned int       ULONG32;
typedef int                LONG32;

typedef union {
    struct { DWORD LowPart; LONG HighPart; };
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef union {
    struct { DWORD LowPart; DWORD HighPart; };
    struct { DWORD LowPart; DWORD HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER;

typedef struct { DWORD dwLowDateTime; DWORD dwHighDateTime; } FILETIME;

typedef struct {
    DWORD     dwPageSize;
    LPVOID    lpMinimumApplicationAddress;
    LPVOID    lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD     dwNumberOfProcessors;
    DWORD     dwProcessorType;
    DWORD     dwAllocationGranularity;
    WORD      wProcessorLevel;
    WORD      wProcessorRevision;
} SYSTEM_INFO;

#define INVALID_HANDLE_VALUE   ((HANDLE)(intptr_t)-1)

#ifndef MAX_PATH
#  ifdef PATH_MAX
#    define MAX_PATH PATH_MAX
#  else
#    define MAX_PATH 4096
#  endif
#endif

#define WAIT_OBJECT_0          0u
#define WAIT_ABANDONED         0x80u
#define WAIT_ABANDONED_0       0x80u
#define WAIT_TIMEOUT           258u
#define WAIT_FAILED            0xFFFFFFFFu
#define STILL_ACTIVE           259u

#define ERROR_SUCCESS          0u
#define ERROR_PATH_NOT_FOUND   3u
#define ERROR_ACCESS_DENIED    5u
#define ERROR_INVALID_HANDLE   6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_GEN_FAILURE      31u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_ALREADY_EXISTS   183u
#define ERROR_TIMEOUT          1460u
#define TIMERR_NOERROR         0u

#define CREATE_SUSPENDED       0x00000004u
#define CREATE_EVENT_MANUAL_RESET 0x00000001u
#define CREATE_EVENT_INITIAL_SET  0x00000002u
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1u

#define THREAD_PRIORITY_IDLE          (-15)
#define THREAD_PRIORITY_LOWEST        (-2)
#define THREAD_PRIORITY_BELOW_NORMAL  (-1)
#define THREAD_PRIORITY_NORMAL        0
#define THREAD_PRIORITY_ABOVE_NORMAL  1
#define THREAD_PRIORITY_HIGHEST       2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_ERROR_RETURN  0x7FFFFFFF

#define IDLE_PRIORITY_CLASS           0x00000040u
#define BELOW_NORMAL_PRIORITY_CLASS   0x00004000u
#define NORMAL_PRIORITY_CLASS         0x00000020u
#define ABOVE_NORMAL_PRIORITY_CLASS   0x00008000u
#define HIGH_PRIORITY_CLASS           0x00000080u
#define REALTIME_PRIORITY_CLASS       0x00000100u

#define MEM_COMMIT                    0x00001000u
#define MEM_RESERVE                   0x00002000u
#define MEM_DECOMMIT                  0x00004000u
#define MEM_RELEASE                   0x00008000u
#define MEM_RESET                     0x00080000u
#define MEM_TOP_DOWN                  0x00100000u
#define PAGE_NOACCESS                 0x01u
#define PAGE_READONLY                 0x02u
#define PAGE_READWRITE                0x04u
#define PAGE_WRITECOPY                0x08u
#define PAGE_EXECUTE                  0x10u
#define PAGE_EXECUTE_READ             0x20u
#define PAGE_EXECUTE_READWRITE        0x40u
#define PAGE_EXECUTE_WRITECOPY        0x80u
#define PAGE_GUARD                    0x100u
#define PAGE_NOCACHE                  0x200u

/* ---------------------------------------------------------------------------
 * Last error (thread-local, in win32_compat.c)
 * -----------------------------------------------------------------------*/
DWORD GetLastError(void);
void  SetLastError(DWORD err);

/* ---------------------------------------------------------------------------
 * Lazy, race-free materialisation of a pthread object into a void* slot
 *
 * __atomic builtins rather than C11 <stdatomic.h>: _Atomic and
 * atomic_load_explicit are C-only spellings, so the C11 version restricted
 * this whole header to .c files. The PPU boot scaffold is C++, and including
 * it from there failed under GCC (Clang accepts _Atomic in C++ as an
 * extension, which is exactly the kind of difference that hides until the
 * other compiler tries). The builtins mean the same thing in both languages.
 * -----------------------------------------------------------------------*/
static inline void* ps3_lazy_obj(void** slot, size_t sz,
                                 void (*init)(void*))
{
    void* cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (cur) return cur;
    void* fresh = calloc(1, sz);
    if (!fresh) return NULL;
    init(fresh);
    void* expected = NULL;
    if (__atomic_compare_exchange_n(slot, &expected, fresh, 0 /* strong */,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    free(fresh);                 /* lost the race; use the winner's object */
    return expected;
}

/* Absolute CLOCK_REALTIME deadline `ms` from now, for pthread_cond_timedwait.
 * REALTIME rather than MONOTONIC because Darwin has no
 * pthread_condattr_setclock; a wall-clock step during a wait is the accepted
 * price of one code path on every host. */
static inline struct timespec ps3__deadline_ms(DWORD ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return ts;
}

/* ---------------------------------------------------------------------------
 * Interlocked operations and barriers
 *
 * Every one is a sequentially consistent RMW through the __atomic builtins,
 * which is what the MSVC intrinsics (full-barrier forms) mean. The return
 * conventions are Win32's: Increment/Decrement/Add return the NEW value,
 * Exchange/ExchangeAdd/And/Or/Xor/CompareExchange return the PREVIOUS one.
 *
 * The public names accept a pointer to LONG (32-bit), to `long` or to
 * `long long`, and operate at the width of the object they are given. Code
 * written for MSVC spells interlocked targets `long` as often as LONG,
 * because there the two are the same 32-bit type; on an LP64 host a `long`
 * is 8 bytes, and the only correct atomic on an 8-byte object is an 8-byte
 * one. A LONG target still gets a 4-byte operation, so a
 * `(volatile LONG*)&some_u32` cast keeps meaning what it says. C selects by
 * _Generic, C++ by overloading; the ps3__ilk* functions are the bodies.
 * -----------------------------------------------------------------------*/
#define PS3__ILK_DEFINE(SUF, T) \
static inline T ps3__ilk_inc_##SUF(volatile T* t)            { return __atomic_add_fetch(t, 1, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_dec_##SUF(volatile T* t)            { return __atomic_sub_fetch(t, 1, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_xchg_##SUF(volatile T* t, T v)      { return __atomic_exchange_n(t, v, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_xadd_##SUF(volatile T* t, T v)      { return __atomic_fetch_add(t, v, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_add_##SUF(volatile T* t, T v)       { return __atomic_add_fetch(t, v, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_and_##SUF(volatile T* t, T v)       { return __atomic_fetch_and(t, v, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_or_##SUF(volatile T* t, T v)        { return __atomic_fetch_or(t, v, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_xor_##SUF(volatile T* t, T v)       { return __atomic_fetch_xor(t, v, __ATOMIC_SEQ_CST); } \
static inline T ps3__ilk_cas_##SUF(volatile T* t, T x, T c)  \
{ __atomic_compare_exchange_n(t, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }

PS3__ILK_DEFINE(i, int)          /* LONG */
PS3__ILK_DEFINE(l, long)
PS3__ILK_DEFINE(ll, long long)   /* LONG64 */
#undef PS3__ILK_DEFINE

#ifdef __cplusplus
/* Overloads cannot carry C linkage, and this section sits inside extern "C". */
extern "C++" {
#define PS3__ILK_OVERLOADS(NAME, OP) \
static inline int       NAME(volatile int* t)       { return ps3__ilk_##OP##_i(t); } \
static inline long      NAME(volatile long* t)      { return ps3__ilk_##OP##_l(t); } \
static inline long long NAME(volatile long long* t) { return ps3__ilk_##OP##_ll(t); }
#define PS3__ILK_OVERLOADS2(NAME, OP) \
static inline int       NAME(volatile int* t, int v)             { return ps3__ilk_##OP##_i(t, v); } \
static inline long      NAME(volatile long* t, long v)           { return ps3__ilk_##OP##_l(t, v); } \
static inline long long NAME(volatile long long* t, long long v) { return ps3__ilk_##OP##_ll(t, v); }
#define PS3__ILK_OVERLOADS3(NAME) \
static inline int       NAME(volatile int* t, int x, int c)                        { return ps3__ilk_cas_i(t, x, c); } \
static inline long      NAME(volatile long* t, long x, long c)                     { return ps3__ilk_cas_l(t, x, c); } \
static inline long long NAME(volatile long long* t, long long x, long long c)      { return ps3__ilk_cas_ll(t, x, c); }
PS3__ILK_OVERLOADS(InterlockedIncrement, inc)
PS3__ILK_OVERLOADS(InterlockedDecrement, dec)
PS3__ILK_OVERLOADS2(InterlockedExchange, xchg)
PS3__ILK_OVERLOADS2(InterlockedExchangeAdd, xadd)
PS3__ILK_OVERLOADS2(InterlockedAdd, add)
PS3__ILK_OVERLOADS2(InterlockedAnd, and)
PS3__ILK_OVERLOADS2(InterlockedOr, or)
PS3__ILK_OVERLOADS2(InterlockedXor, xor)
PS3__ILK_OVERLOADS3(InterlockedCompareExchange)
/* The 64 names: LONG64 is long long, and int64_t is `long` on Linux. */
static inline long      InterlockedIncrement64(volatile long* t)                { return ps3__ilk_inc_l(t); }
static inline long long InterlockedIncrement64(volatile long long* t)           { return ps3__ilk_inc_ll(t); }
static inline long      InterlockedDecrement64(volatile long* t)                { return ps3__ilk_dec_l(t); }
static inline long long InterlockedDecrement64(volatile long long* t)           { return ps3__ilk_dec_ll(t); }
static inline long      InterlockedExchange64(volatile long* t, long v)         { return ps3__ilk_xchg_l(t, v); }
static inline long long InterlockedExchange64(volatile long long* t, long long v){ return ps3__ilk_xchg_ll(t, v); }
static inline long      InterlockedExchangeAdd64(volatile long* t, long v)      { return ps3__ilk_xadd_l(t, v); }
static inline long long InterlockedExchangeAdd64(volatile long long* t, long long v){ return ps3__ilk_xadd_ll(t, v); }
static inline long      InterlockedAdd64(volatile long* t, long v)              { return ps3__ilk_add_l(t, v); }
static inline long long InterlockedAdd64(volatile long long* t, long long v)    { return ps3__ilk_add_ll(t, v); }
static inline long      InterlockedAnd64(volatile long* t, long v)              { return ps3__ilk_and_l(t, v); }
static inline long long InterlockedAnd64(volatile long long* t, long long v)    { return ps3__ilk_and_ll(t, v); }
static inline long      InterlockedOr64(volatile long* t, long v)               { return ps3__ilk_or_l(t, v); }
static inline long long InterlockedOr64(volatile long long* t, long long v)     { return ps3__ilk_or_ll(t, v); }
static inline long      InterlockedXor64(volatile long* t, long v)              { return ps3__ilk_xor_l(t, v); }
static inline long long InterlockedXor64(volatile long long* t, long long v)    { return ps3__ilk_xor_ll(t, v); }
static inline long      InterlockedCompareExchange64(volatile long* t, long x, long c) { return ps3__ilk_cas_l(t, x, c); }
static inline long long InterlockedCompareExchange64(volatile long long* t, long long x, long long c) { return ps3__ilk_cas_ll(t, x, c); }
#undef PS3__ILK_OVERLOADS
#undef PS3__ILK_OVERLOADS2
#undef PS3__ILK_OVERLOADS3
} /* extern "C++" */
#else /* C */
#define PS3__ILK_PICK(p, OP) _Generic((p), \
    volatile int*:       ps3__ilk_##OP##_i,  int*:       ps3__ilk_##OP##_i,  \
    volatile long*:      ps3__ilk_##OP##_l,  long*:      ps3__ilk_##OP##_l,  \
    volatile long long*: ps3__ilk_##OP##_ll, long long*: ps3__ilk_##OP##_ll)
#define PS3__ILK_PICK64(p, OP) _Generic((p), \
    volatile long*:      ps3__ilk_##OP##_l,  long*:      ps3__ilk_##OP##_l,  \
    volatile long long*: ps3__ilk_##OP##_ll, long long*: ps3__ilk_##OP##_ll)
#define InterlockedIncrement(p)            PS3__ILK_PICK((p), inc)((p))
#define InterlockedDecrement(p)            PS3__ILK_PICK((p), dec)((p))
#define InterlockedExchange(p, v)          PS3__ILK_PICK((p), xchg)((p), (v))
#define InterlockedExchangeAdd(p, v)       PS3__ILK_PICK((p), xadd)((p), (v))
#define InterlockedAdd(p, v)               PS3__ILK_PICK((p), add)((p), (v))
#define InterlockedAnd(p, v)               PS3__ILK_PICK((p), and)((p), (v))
#define InterlockedOr(p, v)                PS3__ILK_PICK((p), or)((p), (v))
#define InterlockedXor(p, v)               PS3__ILK_PICK((p), xor)((p), (v))
#define InterlockedCompareExchange(p, x, c) PS3__ILK_PICK((p), cas)((p), (x), (c))
#define InterlockedIncrement64(p)          PS3__ILK_PICK64((p), inc)((p))
#define InterlockedDecrement64(p)          PS3__ILK_PICK64((p), dec)((p))
#define InterlockedExchange64(p, v)        PS3__ILK_PICK64((p), xchg)((p), (v))
#define InterlockedExchangeAdd64(p, v)     PS3__ILK_PICK64((p), xadd)((p), (v))
#define InterlockedAdd64(p, v)             PS3__ILK_PICK64((p), add)((p), (v))
#define InterlockedAnd64(p, v)             PS3__ILK_PICK64((p), and)((p), (v))
#define InterlockedOr64(p, v)              PS3__ILK_PICK64((p), or)((p), (v))
#define InterlockedXor64(p, v)             PS3__ILK_PICK64((p), xor)((p), (v))
#define InterlockedCompareExchange64(p, x, c) PS3__ILK_PICK64((p), cas)((p), (x), (c))
#endif

static inline PVOID InterlockedExchangePointer(PVOID volatile* t, PVOID v) { return __atomic_exchange_n(t, v, __ATOMIC_SEQ_CST); }
static inline PVOID InterlockedCompareExchangePointer(PVOID volatile* t, PVOID exchange, PVOID comparand)
{
    __atomic_compare_exchange_n(t, &comparand, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}

/* The intrinsic spellings are the same operations. */
#define _InterlockedIncrement          InterlockedIncrement
#define _InterlockedDecrement          InterlockedDecrement
#define _InterlockedExchange           InterlockedExchange
#define _InterlockedExchangeAdd        InterlockedExchangeAdd
#define _InterlockedAnd                InterlockedAnd
#define _InterlockedOr                 InterlockedOr
#define _InterlockedXor                InterlockedXor
#define _InterlockedCompareExchange    InterlockedCompareExchange
#define _InterlockedIncrement64        InterlockedIncrement64
#define _InterlockedDecrement64        InterlockedDecrement64
#define _InterlockedExchange64         InterlockedExchange64
#define _InterlockedExchangeAdd64      InterlockedExchangeAdd64
#define _InterlockedAnd64              InterlockedAnd64
#define _InterlockedOr64               InterlockedOr64
#define _InterlockedXor64              InterlockedXor64
#define _InterlockedCompareExchange64  InterlockedCompareExchange64
#define _InterlockedExchangePointer    InterlockedExchangePointer
#define _InterlockedCompareExchangePointer InterlockedCompareExchangePointer

/* Acquire/release loads and stores (winnt.h), for the code that publishes a
 * flag after its data rather than through an RMW. */
static inline LONG   ReadAcquire(const volatile LONG* p)          { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline LONG   ReadNoFence(const volatile LONG* p)          { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline void   WriteRelease(volatile LONG* p, LONG v)       { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline void   WriteNoFence(volatile LONG* p, LONG v)       { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
static inline LONG64 ReadAcquire64(const volatile LONG64* p)      { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline LONG64 ReadNoFence64(const volatile LONG64* p)      { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline void   WriteRelease64(volatile LONG64* p, LONG64 v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline void   WriteNoFence64(volatile LONG64* p, LONG64 v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
static inline PVOID  ReadPointerAcquire(PVOID const volatile* p)  { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void   WritePointerRelease(PVOID volatile* p, PVOID v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

/* Full hardware fence vs. compiler-only fence: MSVC's MemoryBarrier is the
 * former, _ReadWriteBarrier the latter. Keep them distinct -- collapsing both
 * to a compiler fence is exactly what x86-TSO lets you get away with and arm64
 * does not. */
static inline void MemoryBarrier(void)     { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void _ReadWriteBarrier(void) { __asm__ __volatile__("" ::: "memory"); }
static inline void _ReadBarrier(void)      { __asm__ __volatile__("" ::: "memory"); }
static inline void _WriteBarrier(void)     { __asm__ __volatile__("" ::: "memory"); }

/* Spin hint: tells the core this is a wait loop, so it can back off rather
 * than burn the pipeline. Purely advisory -- doing nothing is correct, just
 * slower, which is why the fallback is empty rather than a syscall. */
static inline void YieldProcessor(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

/* ---------------------------------------------------------------------------
 * SRW locks: pointer-sized slot, pthread_rwlock_t materialised on first use.
 *
 * Both lock modes are supported. The Shared variants were absent while only
 * the Exclusive ones were used in-tree; the game runners this header exists
 * for take reader locks on their residency tables, so the mapping had to grow
 * into a real reader-writer lock rather than a mutex.
 * -----------------------------------------------------------------------*/
typedef void*  SRWLOCK;
typedef SRWLOCK* PSRWLOCK;
#define SRWLOCK_INIT NULL

static inline void ps3__rwl_init(void* p) { pthread_rwlock_init((pthread_rwlock_t*)p, NULL); }
static inline pthread_rwlock_t* ps3_srw(SRWLOCK* l)
{ return (pthread_rwlock_t*)ps3_lazy_obj((void**)l, sizeof(pthread_rwlock_t), ps3__rwl_init); }

static inline void InitializeSRWLock(SRWLOCK* l)          { *l = NULL; }
static inline void AcquireSRWLockExclusive(SRWLOCK* l)    { pthread_rwlock_wrlock(ps3_srw(l)); }
static inline void ReleaseSRWLockExclusive(SRWLOCK* l)    { pthread_rwlock_unlock(ps3_srw(l)); }
static inline BOOL TryAcquireSRWLockExclusive(SRWLOCK* l) { return pthread_rwlock_trywrlock(ps3_srw(l)) == 0; }
static inline void AcquireSRWLockShared(SRWLOCK* l)       { pthread_rwlock_rdlock(ps3_srw(l)); }
static inline void ReleaseSRWLockShared(SRWLOCK* l)       { pthread_rwlock_unlock(ps3_srw(l)); }
static inline BOOL TryAcquireSRWLockShared(SRWLOCK* l)    { return pthread_rwlock_tryrdlock(ps3_srw(l)) == 0; }

/* ---------------------------------------------------------------------------
 * Critical sections: a recursive mutex, as on Windows.
 *
 * Not a void* slot: CRITICAL_SECTION is an out-of-line struct on Windows too,
 * always initialised through InitializeCriticalSection, so the real object
 * can live inline here.
 * -----------------------------------------------------------------------*/
typedef struct {
    pthread_mutex_t m;
    int             initialised;
} CRITICAL_SECTION;
typedef CRITICAL_SECTION* LPCRITICAL_SECTION;
typedef CRITICAL_SECTION* PCRITICAL_SECTION;

static inline void InitializeCriticalSection(CRITICAL_SECTION* cs)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->m, &a);
    pthread_mutexattr_destroy(&a);
    cs->initialised = 1;
}
static inline BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* cs, DWORD spin)
{ (void)spin; InitializeCriticalSection(cs); return TRUE; }
static inline BOOL InitializeCriticalSectionEx(CRITICAL_SECTION* cs, DWORD spin, DWORD flags)
{ (void)spin; (void)flags; InitializeCriticalSection(cs); return TRUE; }
static inline void EnterCriticalSection(CRITICAL_SECTION* cs)    { pthread_mutex_lock(&cs->m); }
static inline void LeaveCriticalSection(CRITICAL_SECTION* cs)    { pthread_mutex_unlock(&cs->m); }
static inline BOOL TryEnterCriticalSection(CRITICAL_SECTION* cs) { return pthread_mutex_trylock(&cs->m) == 0; }
static inline void DeleteCriticalSection(CRITICAL_SECTION* cs)
{ if (cs->initialised) { pthread_mutex_destroy(&cs->m); cs->initialised = 0; } }

/* ---------------------------------------------------------------------------
 * Condition variables: pointer-sized slot; the object is a mutex, a condvar
 * and a generation counter.
 *
 * Win32 condition variables sleep against an SRW lock in either mode or a
 * critical section, and pthread_cond_wait only knows mutexes. So the wait does
 * not hand the caller's lock to pthread at all: it records the generation
 * under the object's own mutex, releases the caller's lock, sleeps until the
 * generation moves (or the deadline passes), then re-takes the caller's lock
 * in the mode it was held. Wake bumps the generation under the same mutex.
 * There is no lost wakeup: the caller holds its lock while the generation is
 * read, and a waker must hold that lock to change the predicate, so its bump
 * is ordered after the read. Spurious wakeups are allowed, as on Windows.
 * -----------------------------------------------------------------------*/
typedef void*  CONDITION_VARIABLE;
typedef CONDITION_VARIABLE* PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT NULL

typedef struct { pthread_mutex_t m; pthread_cond_t c; unsigned gen; } ps3_condvar;

static inline void ps3__cv_init(void* p)
{
    ps3_condvar* cv = (ps3_condvar*)p;
    pthread_mutex_init(&cv->m, NULL);
    pthread_cond_init(&cv->c, NULL);
    cv->gen = 0;
}
static inline ps3_condvar* ps3_cv(CONDITION_VARIABLE* c)
{ return (ps3_condvar*)ps3_lazy_obj((void**)c, sizeof(ps3_condvar), ps3__cv_init); }

static inline void InitializeConditionVariable(CONDITION_VARIABLE* c) { *c = NULL; }
static inline void WakeConditionVariable(CONDITION_VARIABLE* c)
{
    ps3_condvar* cv = ps3_cv(c);
    if (!cv) return;
    pthread_mutex_lock(&cv->m);
    cv->gen++;
    pthread_cond_signal(&cv->c);
    pthread_mutex_unlock(&cv->m);
}
static inline void WakeAllConditionVariable(CONDITION_VARIABLE* c)
{
    ps3_condvar* cv = ps3_cv(c);
    if (!cv) return;
    pthread_mutex_lock(&cv->m);
    cv->gen++;
    pthread_cond_broadcast(&cv->c);
    pthread_mutex_unlock(&cv->m);
}

/* The wait body, shared by the SRW and CS entry points. `unlock`/`relock`
 * are the caller's lock in the mode it holds it. */
static inline BOOL ps3__cv_sleep(ps3_condvar* cv, DWORD ms,
                                 void (*unlock)(void*), void (*relock)(void*), void* lock)
{
    if (!cv) return FALSE;
    pthread_mutex_lock(&cv->m);
    unsigned g = cv->gen;
    unlock(lock);
    int rc = 0;
    if (ms == INFINITE) {
        while (cv->gen == g && rc == 0) rc = pthread_cond_wait(&cv->c, &cv->m);
    } else {
        struct timespec ts = ps3__deadline_ms(ms);
        while (cv->gen == g && rc == 0) rc = pthread_cond_timedwait(&cv->c, &cv->m, &ts);
    }
    BOOL woke = (cv->gen != g);
    pthread_mutex_unlock(&cv->m);
    relock(lock);
    if (!woke) SetLastError(ERROR_TIMEOUT);
    return woke;
}
static inline void ps3__srw_unlock_x(void* l) { ReleaseSRWLockExclusive((SRWLOCK*)l); }
static inline void ps3__srw_lock_x  (void* l) { AcquireSRWLockExclusive((SRWLOCK*)l); }
static inline void ps3__srw_unlock_s(void* l) { ReleaseSRWLockShared((SRWLOCK*)l); }
static inline void ps3__srw_lock_s  (void* l) { AcquireSRWLockShared((SRWLOCK*)l); }
static inline void ps3__cs_unlock   (void* l) { LeaveCriticalSection((CRITICAL_SECTION*)l); }
static inline void ps3__cs_lock     (void* l) { EnterCriticalSection((CRITICAL_SECTION*)l); }

static inline BOOL SleepConditionVariableSRW(CONDITION_VARIABLE* c, SRWLOCK* l, DWORD ms, ULONG flags)
{
    if (flags & CONDITION_VARIABLE_LOCKMODE_SHARED)
        return ps3__cv_sleep(ps3_cv(c), ms, ps3__srw_unlock_s, ps3__srw_lock_s, l);
    return ps3__cv_sleep(ps3_cv(c), ms, ps3__srw_unlock_x, ps3__srw_lock_x, l);
}
static inline BOOL SleepConditionVariableCS(CONDITION_VARIABLE* c, CRITICAL_SECTION* cs, DWORD ms)
{
    return ps3__cv_sleep(ps3_cv(c), ms, ps3__cs_unlock, ps3__cs_lock, cs);
}

/* ---------------------------------------------------------------------------
 * One-time initialisation
 *
 * Not pthread_once, which cannot express either half of the Win32 contract.
 * Its callback takes no argument, so the Parameter the caller hands the
 * callback has nowhere to go and the Context a successful callback publishes
 * has nowhere to come from; and its callback returns nothing, so a failed
 * initialisation cannot be reported. Win32 treats FALSE from the callback as
 * "not initialised after all" and leaves the INIT_ONCE where it was, so the
 * next caller tries again -- a lazy open that failed on a missing file gets
 * another go once the file is there. pthread_once has already burnt its flag
 * by then and never calls anything again.
 *
 * So the slot holds a pointer to a small object of the shim's own, materialised
 * on first use exactly as the SRW locks and condition variables above are, and
 * the state machine lives in there: one thread claims the RUNNING state and
 * runs the callback with the lock DROPPED, every other sleeps on the condition
 * variable until the result is known. Dropping the lock is what lets the
 * callback take locks of its own, which is the whole point of the primitive --
 * the callback this exists for constructs a critical section. Re-entering the
 * SAME INIT_ONCE from inside its own callback deadlocks, as it does on Windows.
 *
 * The object is never freed. A Win32 INIT_ONCE has no destroy call, so there is
 * nowhere to hang one, and the callers are process-lifetime statics; this is
 * the same deliberate leak the SRWLOCK and CONDITION_VARIABLE slots take.
 * -----------------------------------------------------------------------*/
typedef union { PVOID Ptr; } INIT_ONCE;
typedef INIT_ONCE* PINIT_ONCE;
typedef INIT_ONCE* LPINIT_ONCE;
#define INIT_ONCE_STATIC_INIT { NULL }

typedef BOOL (WINAPI *PINIT_ONCE_FN)(PINIT_ONCE once, PVOID parameter, PVOID* context);

enum { PS3_ONCE_IDLE = 0, PS3_ONCE_RUNNING = 1, PS3_ONCE_DONE = 2 };

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             state;
    PVOID           context;
} ps3_init_once;

static inline void ps3__once_init(void* p)
{
    ps3_init_once* o = (ps3_init_once*)p;
    pthread_mutex_init(&o->m, NULL);
    pthread_cond_init(&o->c, NULL);
    o->state   = PS3_ONCE_IDLE;
    o->context = NULL;
}
static inline ps3_init_once* ps3_once(INIT_ONCE* once)
{ return (ps3_init_once*)ps3_lazy_obj((void**)&once->Ptr, sizeof(ps3_init_once), ps3__once_init); }

static inline void InitOnceInitialize(PINIT_ONCE once) { once->Ptr = NULL; }

static inline BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn,
                                       PVOID parameter, PVOID* context)
{
    ps3_init_once* o = (once && fn) ? ps3_once(once) : NULL;
    if (!o) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

    pthread_mutex_lock(&o->m);
    while (o->state == PS3_ONCE_RUNNING) pthread_cond_wait(&o->c, &o->m);
    if (o->state == PS3_ONCE_DONE) {
        PVOID done = o->context;
        pthread_mutex_unlock(&o->m);
        if (context) *context = done;
        return TRUE;
    }
    o->state = PS3_ONCE_RUNNING;
    pthread_mutex_unlock(&o->m);

    PVOID produced = NULL;
    BOOL  ok = fn(once, parameter, &produced);

    pthread_mutex_lock(&o->m);
    o->state = ok ? PS3_ONCE_DONE : PS3_ONCE_IDLE;
    if (ok) o->context = produced;
    pthread_cond_broadcast(&o->c);
    pthread_mutex_unlock(&o->m);

    /* A failed callback is expected to have set the error itself, so nothing
     * here overwrites it -- same as Win32. */
    if (!ok) return FALSE;
    if (context) *context = produced;
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * Timing
 * -----------------------------------------------------------------------*/
static inline unsigned long long ps3__mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}
static inline ULONGLONG GetTickCount64(void) { return ps3__mono_ns() / 1000000ull; }
static inline DWORD     GetTickCount(void)   { return (DWORD)(ps3__mono_ns() / 1000000ull); }
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000000LL; return TRUE; }
static inline BOOL QueryPerformanceCounter  (LARGE_INTEGER* c) { c->QuadPart = (LONGLONG)ps3__mono_ns(); return TRUE; }

/* 100 ns units since 1601-01-01, as GetSystemTimeAsFileTime reports. */
static inline void GetSystemTimeAsFileTime(FILETIME* ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unsigned long long t = (unsigned long long)ts.tv_sec * 10000000ull
                         + (unsigned long long)ts.tv_nsec / 100ull
                         + 116444736000000000ull;
    ft->dwLowDateTime  = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}
static inline void GetSystemTimePreciseAsFileTime(FILETIME* ft) { GetSystemTimeAsFileTime(ft); }

static inline void Sleep(DWORD ms)
{
    if (ms == 0) { sched_yield(); return; }      /* Sleep(0) yields, as on Windows */
    struct timespec d = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&d, NULL);
}
static inline DWORD SleepEx(DWORD ms, BOOL alertable) { (void)alertable; Sleep(ms); return 0; }
static inline BOOL  SwitchToThread(void)              { sched_yield(); return TRUE; }
static inline UINT  timeBeginPeriod(UINT period)      { (void)period; return TIMERR_NOERROR; }
static inline UINT  timeEndPeriod(UINT period)        { (void)period; return TIMERR_NOERROR; }

/* A stable, per-thread integer id. Only ever logged or hashed, never handed
 * back to the OS, so any injective-per-thread value will do. Darwin and Linux
 * both offer the real kernel tid, which is what a debugger and the guest-side
 * logs show; elsewhere pthread_self is the only portable handle there is.
 *
 * pthread_threadid_np is Darwin-only. Calling it unguarded built fine under
 * GCC -- an implicit declaration into a static archive, which links nothing --
 * and only failed once Clang rejected it. Hence the CI symbol check. */
static inline DWORD GetCurrentThreadId(void)
{
#if defined(__APPLE__)
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (DWORD)tid;
#elif defined(__linux__)
    /* Not gettid(3): that wrapper only appeared in glibc 2.30. */
    return (DWORD)syscall(SYS_gettid);
#else
    return (DWORD)(uintptr_t)pthread_self();
#endif
}
static inline DWORD  GetCurrentProcessId(void) { return (DWORD)getpid(); }
static inline HANDLE GetCurrentProcess(void)   { return (HANDLE)(intptr_t)-1; }   /* pseudo-handle */
static inline HANDLE GetCurrentThread(void)    { return (HANDLE)(intptr_t)-2; }   /* pseudo-handle */

/* ---------------------------------------------------------------------------
 * Waitable handles: events, semaphores, threads, waitable timers
 *
 * Implemented in win32_compat.c. Every object shares one lock, which is what
 * makes WaitForMultipleObjects(bWaitAll) atomic across objects the way Win32
 * defines it; per-object condition variables keep single waits targeted. The
 * runtime's own hot paths use pthreads directly in their POSIX branches, so
 * the shared lock only carries the coarse, Win32-shaped traffic these were
 * written for.
 *
 * Threads: the handle is joinable (WaitForSingleObject blocks until the
 * routine returns, GetExitCodeThread reads its DWORD), and it is reference
 * counted -- CloseHandle before or after the thread ends is fine either way.
 * CREATE_SUSPENDED parks the thread on a gate that ResumeThread opens; a
 * RUNNING thread is stopped for real, through the mechanism described under
 * "Thread control" below.
 * -----------------------------------------------------------------------*/
typedef DWORD (*PS3_THREAD_FN)(LPVOID);
typedef PS3_THREAD_FN LPTHREAD_START_ROUTINE;

HANDLE CreateEventA(void* sa, BOOL manual_reset, BOOL initial_state, LPCSTR name);
HANDLE CreateEventW(void* sa, BOOL manual_reset, BOOL initial_state, LPCWSTR name);
HANDLE CreateEventExA(void* sa, LPCSTR name, DWORD flags, DWORD access);
#define CreateEvent CreateEventA
BOOL   SetEvent(HANDLE h);
BOOL   ResetEvent(HANDLE h);
BOOL   PulseEvent(HANDLE h);

HANDLE CreateSemaphoreA(void* sa, LONG initial, LONG maximum, LPCSTR name);
HANDLE CreateSemaphoreW(void* sa, LONG initial, LONG maximum, LPCWSTR name);
HANDLE CreateSemaphoreExA(void* sa, LONG initial, LONG maximum, LPCSTR name, DWORD flags, DWORD access);
#define CreateSemaphore CreateSemaphoreA
BOOL   ReleaseSemaphore(HANDLE h, LONG count, LONG* previous);

HANDLE CreateWaitableTimerA(void* sa, BOOL manual_reset, LPCSTR name);
HANDLE CreateWaitableTimerW(void* sa, BOOL manual_reset, LPCWSTR name);
#define CreateWaitableTimer CreateWaitableTimerA
/* due: negative = relative, in 100 ns units (the only form used in-tree);
 * positive = absolute FILETIME. period in ms, 0 = one-shot. */
BOOL   SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period_ms,
                        void* completion, void* arg, BOOL resume);
BOOL   CancelWaitableTimer(HANDLE h);

HANDLE CreateThread(void* sa, SIZE_T stack, PS3_THREAD_FN fn, LPVOID arg,
                    DWORD flags, DWORD* out_tid);
uintptr_t _beginthreadex(void* sa, unsigned stack, unsigned (*fn)(void*),
                         void* arg, unsigned flags, unsigned* out_tid);
void   ExitThread(DWORD code);
void   _endthreadex(unsigned code);
BOOL   GetExitCodeThread(HANDLE h, DWORD* code);
DWORD  ResumeThread(HANDLE h);
DWORD  SuspendThread(HANDLE h);
BOOL   SetThreadPriority(HANDLE h, int priority);
int    GetThreadPriority(HANDLE h);
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask);
DWORD  SetThreadIdealProcessor(HANDLE h, DWORD ideal);
LONG   SetThreadDescription(HANDLE h, PCWSTR description);   /* HRESULT */
BOOL   SetPriorityClass(HANDLE process, DWORD cls);
DWORD  GetPriorityClass(HANDLE process);

DWORD  WaitForSingleObject(HANDLE h, DWORD ms);
DWORD  WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable);
DWORD  WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD ms);
BOOL   CloseHandle(HANDLE h);

/* ---------------------------------------------------------------------------
 * Thread context: the register file
 *
 * Two things here want a thread's registers: the exception layer further down,
 * which hands its handlers the state at the fault, and GetThreadContext on a
 * suspended thread. One struct serves both.
 * -----------------------------------------------------------------------*/
#define CONTEXT_CONTROL         0x00000001u   /* Pc, Sp, Fp, Lr and the flags */
#define CONTEXT_INTEGER         0x00000002u   /* the general registers */
#define CONTEXT_SEGMENTS        0x00000004u
#define CONTEXT_FLOATING_POINT  0x00000008u
#define CONTEXT_DEBUG_REGISTERS 0x00000010u
#define CONTEXT_FULL            (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS)
#define CONTEXT_ALL             (CONTEXT_FULL | CONTEXT_FLOATING_POINT | CONTEXT_DEBUG_REGISTERS)

/* The register file, in the shape both hosts can actually fill.
 *
 * Pc / Sp / Fp / Lr are the portable spelling and are what code meant to build
 * on both architectures should use. On x86-64 they are unions with Rip / Rsp /
 * Rbp so a body written against the Win32 CONTEXT compiles unchanged; Lr has
 * no x86 counterpart and reads zero there. Floating point, vector and segment
 * state are not carried -- nothing that asks for a thread's registers on this
 * path has wanted them -- and neither are the x86 debug registers, which are a
 * separate Mach state flavour and have no arm64 equivalent at all.
 *
 * ContextFlags on input says which groups the caller wants. Everything listed
 * here comes back regardless, because one thread_get_state or one signal frame
 * carries all of it; on return ContextFlags says what was actually filled. */
typedef struct {
    DWORD     ContextFlags;
#if defined(__x86_64__)
    DWORD     EFlags;
    ULONGLONG Rax, Rbx, Rcx, Rdx, Rsi, Rdi;
    ULONGLONG R8, R9, R10, R11, R12, R13, R14, R15;
    union { ULONGLONG Pc; ULONGLONG Rip; };
    union { ULONGLONG Sp; ULONGLONG Rsp; };
    union { ULONGLONG Fp; ULONGLONG Rbp; };
    ULONGLONG Lr;                       /* no link register on x86-64: zero */
#elif defined(__aarch64__)
    DWORD     Cpsr;
    ULONGLONG X[29];                    /* x0 .. x28 */
    ULONGLONG Pc;
    ULONGLONG Sp;
    union { ULONGLONG Fp; ULONGLONG X29; };
    union { ULONGLONG Lr; ULONGLONG X30; };
#else
    DWORD     ps3_status;
    ULONGLONG Pc, Sp, Fp, Lr;
#endif
} CONTEXT;
typedef CONTEXT* PCONTEXT;
typedef CONTEXT* LPCONTEXT;

/* ---------------------------------------------------------------------------
 * Thread control: stopping a running thread and reading its registers
 *
 * What a game runner does with these is diagnosis: freeze a thread that looks
 * stuck, read its program counter and stack pointer, walk the host stack, let
 * it go. That is a debugger's job, and both hosts have a debugger's mechanism
 * for it, so this is a real implementation rather than the failure stub the
 * shim used to return.
 *
 *   Darwin: Mach. The thread record keeps the port pthread_mach_thread_np
 *   gives for its pthread, and thread_suspend / thread_resume /
 *   thread_get_state / thread_set_state do the work -- exactly what a debugger
 *   attached to the process would issue. thread_suspend is synchronous: when
 *   it returns the thread executes no further user instructions.
 *
 *   Linux: a real-time signal (SIGRTMIN+4, out of the range glibc and musl
 *   reserve for their own use). The handler saves the interrupted registers
 *   into the thread record and parks on a semaphore until the resume; the
 *   registers a later SetThreadContext writes are put back into the signal
 *   frame on the way out, so returning from the handler resumes the thread on
 *   them. sem_post and sem_wait are async-signal-safe, which is what makes the
 *   park legal inside a handler; nothing in it takes the shim's lock. A thread
 *   that blocks SIGRTMIN+4 cannot be stopped this way, and SuspendThread waits
 *   for it: do not mask it in a thread anything might want to freeze.
 *
 * The Win32 hazards apply unchanged and are inherent, not artefacts of the
 * mapping: a thread stopped inside malloc still holds malloc's lock, so the
 * suspending thread must not allocate before it resumes it. Suspending the
 * CALLING thread is refused ((DWORD)-1) rather than deadlocking the shim.
 *
 * Suspend counts nest, as on Windows: SuspendThread returns the PREVIOUS
 * count and ResumeThread the previous count too, and the thread runs again
 * only when the count reaches zero. A thread created CREATE_SUSPENDED starts
 * at one and is parked on the gate in the trampoline instead, which is where
 * these two counts meet: the same counter drives both.
 *
 * A thread the shim did not create -- the process's main thread, or one a
 * library spawned -- is ADOPTED into the thread table the first time one of
 * these calls names it, so DuplicateHandle(GetCurrentThread()) from main()
 * yields a handle the rest of them accept. An adopted record has no exit code
 * and never becomes signaled: nothing tells the shim when a foreign thread
 * ends, so waiting on one waits forever, and the record lives for the process.
 * -----------------------------------------------------------------------*/
#define THREAD_TERMINATE            0x00000001u
#define THREAD_SUSPEND_RESUME       0x00000002u
#define THREAD_GET_CONTEXT          0x00000008u
#define THREAD_SET_CONTEXT          0x00000010u
#define THREAD_SET_INFORMATION      0x00000020u
#define THREAD_QUERY_INFORMATION    0x00000040u
#define SYNCHRONIZE                 0x00100000u
#define THREAD_ALL_ACCESS           0x001FFFFFu

#define DUPLICATE_CLOSE_SOURCE      0x00000001u
#define DUPLICATE_SAME_ACCESS       0x00000002u

/* Meaningful on a SUSPENDED thread. On Linux only a suspended thread has a
 * saved frame at all, so these fail on any other; on Darwin the Mach call
 * answers for a running thread too, and the answer is a snapshot that was
 * already stale when it was taken. */
BOOL   GetThreadContext(HANDLE h, CONTEXT* ctx);
BOOL   SetThreadContext(HANDLE h, const CONTEXT* ctx);

/* Access rights are not enforced: this is one process with one address space,
 * and a handle either names a thread the shim knows or it does not. NULL if
 * no thread in the table has that id. */
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid);

/* Same-process duplication only, which is every use there is here. The source
 * may be the current-thread pseudo handle, and that is the point of it: it is
 * how a thread the shim did not create gets a real, reference-counted handle
 * to itself. Both process arguments must be the current-process pseudo handle. */
BOOL   DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process,
                       HANDLE* out, DWORD access, BOOL inherit, DWORD options);

/* Times in FILETIME units (100 ns). Creation and exit are recorded by the
 * shim, so an adopted thread reports its adoption rather than its birth and
 * zero for an exit that has not happened. Kernel and user are the thread's own
 * CPU time: Darwin splits them (thread_info THREAD_BASIC_INFO), Linux cannot
 * without parsing /proc, so it reports the total as user time and zero kernel. */
BOOL   GetThreadTimes(HANDLE h, FILETIME* creation, FILETIME* exit,
                      FILETIME* kernel, FILETIME* user);

/* A snapshot of the shim's thread table, which is what TH32CS_SNAPTHREAD gets
 * you here: threads the shim created plus every thread since adopted. Threads
 * belonging to the C library or to a framework are invisible, and no other
 * process is ever enumerable. th32OwnerProcessID is always this process. */
#define TH32CS_SNAPTHREAD  0x00000004u

typedef struct {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ThreadID;
    DWORD th32OwnerProcessID;
    LONG  tpBasePri;
    LONG  tpDeltaPri;
    DWORD dwFlags;
} THREADENTRY32;
typedef THREADENTRY32* LPTHREADENTRY32;

HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid);
BOOL   Thread32First(HANDLE snapshot, THREADENTRY32* entry);
BOOL   Thread32Next(HANDLE snapshot, THREADENTRY32* entry);

/* ---------------------------------------------------------------------------
 * WaitOnAddress / WakeByAddress
 *
 * A parking lot: the address hashes to one of a fixed set of mutex+condvar
 * buckets. The waiter compares under the bucket lock and sleeps on it; the
 * waker takes the same lock and broadcasts. That ordering is what rules out a
 * lost wakeup, provided the waker changes the value BEFORE it wakes -- which
 * is what the Win32 contract requires of it too. WakeByAddressSingle also
 * broadcasts: with hashing, a signal could wake a waiter on a different
 * address and leave the intended one asleep, and a spurious return is
 * something every WaitOnAddress caller already re-checks for.
 * -----------------------------------------------------------------------*/
BOOL   WaitOnAddress(volatile VOID* address, PVOID compare, SIZE_T size, DWORD ms);
void   WakeByAddressSingle(PVOID address);
void   WakeByAddressAll(PVOID address);

/* ---------------------------------------------------------------------------
 * Virtual memory
 *
 * MEM_RESERVE maps PROT_NONE (at the requested address if one is given, and
 * fails rather than displacing an existing mapping, as Win32 does); MEM_COMMIT
 * on a reserved range makes it accessible; MEM_RELEASE unmaps a reservation
 * (its size is remembered, since munmap needs one and Win32 callers pass 0).
 * VirtualProtect cannot report the previous protection -- there is no
 * portable page-table read -- so *old is PAGE_READWRITE, and a caller that
 * restores it gets a writable page, which is the safe direction.
 * -----------------------------------------------------------------------*/
LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect);
BOOL   VirtualFree(LPVOID address, SIZE_T size, DWORD type);
BOOL   VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, DWORD* old);
void   GetSystemInfo(SYSTEM_INFO* si);
#define GetNativeSystemInfo GetSystemInfo

/* ---------------------------------------------------------------------------
 * Querying the address space
 *
 * VirtualQuery answers from two sources. The shim's own VirtualAlloc region
 * table gives AllocationBase and AllocationProtect -- the reservation an
 * address belongs to and the protection it was reserved with -- because that
 * is a Win32 notion neither host keeps for us. The OS gives everything else:
 * mach_vm_region on Darwin, /proc/self/maps on Linux, both of which report the
 * run of pages around an address that share one protection, which is exactly
 * what BaseAddress and RegionSize mean.
 *
 * State follows the shim's own model of reserve and commit, since that is what
 * it built the mapping out of: PROT_NONE is MEM_RESERVE, anything accessible
 * is MEM_COMMIT, nothing mapped at all is MEM_FREE. A page the process made
 * PROT_NONE for its own reasons therefore reads as reserved, which is the same
 * conflation Win32 makes with PAGE_NOACCESS on a committed page and the reason
 * MEM_COMMIT is the flag worth testing.
 *
 * Type is MEM_PRIVATE throughout. Distinguishing a file mapping from an
 * anonymous one costs a second query on both hosts and no caller has asked.
 *
 * IsBadReadPtr and IsBadWritePtr walk the range through the same resolution,
 * a whole region at a time, and are safe on an unmapped or protected address
 * because nothing dereferences it. They carry the Win32 caveat too: the answer
 * describes the address space at the moment it was taken, and another thread
 * may unmap the page before the caller reads it.
 * -----------------------------------------------------------------------*/
#define MEM_FREE                      0x00010000u
#define MEM_PRIVATE                   0x00020000u
#define MEM_MAPPED                    0x00040000u
#define MEM_IMAGE                     0x01000000u

typedef struct {
    PVOID  BaseAddress;
    PVOID  AllocationBase;
    DWORD  AllocationProtect;
    SIZE_T RegionSize;
    DWORD  State;
    DWORD  Protect;
    DWORD  Type;
} MEMORY_BASIC_INFORMATION;
typedef MEMORY_BASIC_INFORMATION* PMEMORY_BASIC_INFORMATION;

SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION mbi, SIZE_T length);
BOOL   IsBadReadPtr(LPCVOID p, UINT_PTR length);
BOOL   IsBadWritePtr(LPVOID p, UINT_PTR length);
BOOL   IsBadCodePtr(LPCVOID p);

/* ---------------------------------------------------------------------------
 * Vectored exception handlers
 *
 * A game runner uses these to survive and explain a fault: re-protect a
 * watched page and let the store through, or print the guest function behind a
 * host access violation before the process dies. Both are sigaction work on
 * POSIX, and the translation is mechanical enough to be worth doing once here
 * rather than in every runner.
 *
 * SIGSEGV, SIGBUS, SIGILL and SIGFPE are taken over on the first registration.
 * Each delivery builds an EXCEPTION_RECORD (the code from the signal and its
 * si_code, ExceptionAddress from the interrupted program counter, and for an
 * access violation NumberParameters 2 with ExceptionInformation[0] set to 1
 * for a write and [1] to the faulting address) and a CONTEXT from the signal
 * frame, then runs the handlers in registration order -- first-registered
 * first when AddVectoredExceptionHandler was passed a nonzero `first`, as
 * Win32 defines it.
 *
 *   EXCEPTION_CONTINUE_EXECUTION writes the CONTEXT's Pc and Sp back into the
 *   signal frame and returns, so the thread resumes on them. That is what
 *   makes "re-protect the page and retry the instruction" work.
 *
 *   EXCEPTION_CONTINUE_SEARCH goes to the next handler, then to the unhandled
 *   filter, and then to whatever sigaction was installed before the shim.
 *
 * That last step is the load-bearing one. runtime/ppu/ppu_loader.cpp installs
 * its own SIGSEGV and SIGBUS handlers for the untranslated-guest-pointer report
 * and chains to their predecessors by hand, keeping one saved action per
 * signal; the two compose in either installation order because both keep the
 * previous action and call it in its own form, SIGINFO or not, and both restore
 * SIG_DFL and return when there is nothing to chain to -- which re-runs the
 * faulting instruction and lets the process die exactly as it would have.
 *
 * The dispatcher takes no lock: the handler list is read through atomic loads
 * so a fault on a thread that happens to hold the shim's lock is not a
 * deadlock. Registrations are bounded (64 for the life of the process) and a
 * removed slot is reused only when the next registration wants its position,
 * which covers the arm/disarm cycle a watchpoint does.
 *
 * A stack overflow is not recoverable here unless the process installed a
 * sigaltstack of its own: the handlers are registered SA_ONSTACK, but the shim
 * does not create the alternate stack, so the fault that has no stack left to
 * run a handler on still ends the process.
 * -----------------------------------------------------------------------*/
#define EXCEPTION_MAXIMUM_PARAMETERS    15

#define EXCEPTION_ACCESS_VIOLATION      0xC0000005u
#define EXCEPTION_IN_PAGE_ERROR         0xC0000006u
#define EXCEPTION_ILLEGAL_INSTRUCTION   0xC000001Du
#define EXCEPTION_PRIV_INSTRUCTION      0xC0000096u
#define EXCEPTION_DATATYPE_MISALIGNMENT 0x80000002u
#define EXCEPTION_BREAKPOINT            0x80000003u
#define EXCEPTION_SINGLE_STEP           0x80000004u
#define EXCEPTION_ARRAY_BOUNDS_EXCEEDED 0xC000008Cu
#define EXCEPTION_FLT_DENORMAL_OPERAND  0xC000008Du
#define EXCEPTION_FLT_DIVIDE_BY_ZERO    0xC000008Eu
#define EXCEPTION_FLT_INEXACT_RESULT    0xC000008Fu
#define EXCEPTION_FLT_INVALID_OPERATION 0xC0000090u
#define EXCEPTION_FLT_OVERFLOW          0xC0000091u
#define EXCEPTION_FLT_STACK_CHECK       0xC0000092u
#define EXCEPTION_FLT_UNDERFLOW         0xC0000093u
#define EXCEPTION_INT_DIVIDE_BY_ZERO    0xC0000094u
#define EXCEPTION_INT_OVERFLOW          0xC0000095u
#define EXCEPTION_STACK_OVERFLOW        0xC00000FDu

#define EXCEPTION_CONTINUE_EXECUTION    (-1)
#define EXCEPTION_CONTINUE_SEARCH       0
#define EXCEPTION_EXECUTE_HANDLER       1

#define EXCEPTION_NONCONTINUABLE        0x01u

typedef struct _EXCEPTION_RECORD {
    DWORD                    ExceptionCode;
    DWORD                    ExceptionFlags;
    struct _EXCEPTION_RECORD* ExceptionRecord;
    PVOID                    ExceptionAddress;
    DWORD                    NumberParameters;
    ULONG_PTR                ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD;
typedef EXCEPTION_RECORD* PEXCEPTION_RECORD;

typedef struct {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT          ContextRecord;
} EXCEPTION_POINTERS;
typedef EXCEPTION_POINTERS* PEXCEPTION_POINTERS;
typedef EXCEPTION_POINTERS* LPEXCEPTION_POINTERS;

typedef LONG (WINAPI *PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS*);
typedef LONG (WINAPI *LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS*);
typedef LPTOP_LEVEL_EXCEPTION_FILTER PTOP_LEVEL_EXCEPTION_FILTER;

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
ULONG RemoveVectoredExceptionHandler(PVOID handle);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);

/* ---------------------------------------------------------------------------
 * Odds and ends
 * -----------------------------------------------------------------------*/
static inline void OutputDebugStringA(LPCSTR s) { if (s) fputs(s, stderr); }
static inline BOOL IsDebuggerPresent(void)      { return FALSE; }
static inline void DebugBreak(void)             { __builtin_trap(); }
static inline DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size)
{
    const char* v = name ? getenv(name) : NULL;
    if (!v) return 0;
    size_t n = strlen(v);
    if (!buf || size == 0 || n + 1 > size) return (DWORD)(n + 1);
    memcpy(buf, v, n + 1);
    return (DWORD)n;
}

/* mkdir, wearing Win32's answer to a directory that is already there: the call
 * FAILS and GetLastError says ERROR_ALREADY_EXISTS. That distinction is the
 * whole reason this is not just mkdir at the call site -- code that creates its
 * own dump or cache directory calls unconditionally and tests for exactly that
 * error, and a shim that returned TRUE would also swallow a permission failure.
 * The security-descriptor argument has no POSIX counterpart and is ignored;
 * 0777 leaves the final mode to the caller's umask.
 *
 * Separators are NOT translated. A backslash is an ordinary filename character
 * here, so a Win32-spelt "scratch\\dump" makes one oddly named directory rather
 * than two nested ones. Translating would guess at which backslashes were meant
 * as separators in a name that is allowed to contain them; the call site is
 * what has to spell the path portably. */
static inline BOOL CreateDirectoryA(LPCSTR path, void* security_attributes)
{
    (void)security_attributes;
    if (!path) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (mkdir(path, 0777) == 0) return TRUE;
    switch (errno) {
    case EEXIST:  SetLastError(ERROR_ALREADY_EXISTS); break;
    case ENOENT:
    case ENOTDIR: SetLastError(ERROR_PATH_NOT_FOUND); break;
    case EACCES:
    case EPERM:
    case EROFS:   SetLastError(ERROR_ACCESS_DENIED);  break;
    default:      SetLastError(ERROR_GEN_FAILURE);    break;
    }
    return FALSE;
}

/* Process information classes, of which exactly one is ever set here: a Win32
 * runner opts out of EcoQoS with ProcessPowerThrottling so that backgrounding
 * its window cannot throttle the scheduling its boot races depend on.
 *
 * Neither POSIX host has anything that corresponds. Darwin's equivalent knob is
 * per-thread QoS, which the threads set for themselves through
 * SetThreadPriority, and Linux has no such throttle to opt out of in the first
 * place. So this succeeds and does nothing, deliberately: failing would push a
 * caller down an error path over a facility that does not exist on this host
 * and cannot be made to, and the things that DO move host scheduling -- the
 * timer period, the priority class, thread priority -- are separate calls and
 * are really implemented. Nothing is silently lost by the no-op that those
 * three do not already cover. */
typedef enum {
    ProcessMemoryPriority,
    ProcessMemoryExhaustionInfo,
    ProcessAppMemoryInfo,
    ProcessInPrivateInfo,
    ProcessPowerThrottling,
    ProcessReservedValue1,
    ProcessTelemetryCoverageInfo,
    ProcessProtectionLevelInfo,
    ProcessLeapSecondInfo,
    ProcessMachineTypeInfo,
    ProcessInformationClassMax
} PROCESS_INFORMATION_CLASS;

#define PROCESS_POWER_THROTTLING_CURRENT_VERSION         1u
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED         0x1u
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4u

typedef struct {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} PROCESS_POWER_THROTTLING_STATE;

static inline BOOL SetProcessInformation(HANDLE process, PROCESS_INFORMATION_CLASS cls,
                                         LPVOID info, DWORD size)
{ (void)process; (void)cls; (void)info; (void)size; return TRUE; }

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* !_WIN32 */
#endif /* PS3RECOMP_WIN32_COMPAT_H */

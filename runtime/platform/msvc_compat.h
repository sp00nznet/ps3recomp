/*
 * ps3recomp - MSVC CRT and intrinsic spellings on Clang/GCC
 *
 * The runtime library keeps its platform switches inline (#ifdef _WIN32, see
 * docs/PLATFORM_ABSTRACTION.md). A game runner is a different kind of source:
 * tens of thousands of lines written against MSVC on Windows, where the
 * secure-CRT names (_snprintf, fopen_s, _stricmp), the intrinsics
 * (_byteswap_ulong, _BitScanForward, _ReturnAddress) and __declspec are the
 * dialect the whole file speaks. Fencing each of those hundreds of sites would
 * be the real port; supplying the names lets the file compile first, so the
 * port can be about behaviour instead of spelling.
 *
 * On MSVC this header is a passthrough. It is independent of win32_compat.h
 * (either can be included alone), and the two do not overlap.
 *
 * Widths follow the MSVC declarations, not the LP64 `long`: the bit-scan
 * masks and indices are 32-bit, as they are in <intrin.h>, so a DWORD lands
 * in them unchanged.
 */
#ifndef PS3RECOMP_MSVC_COMPAT_H
#define PS3RECOMP_MSVC_COMPAT_H

#ifdef _MSC_VER
#  include <intrin.h>
#  include <stdlib.h>
#  include <string.h>
#  include <direct.h>
#  include <io.h>
#else

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__linux__)
#  include <alloca.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- storage-class and calling-convention keywords ------------------------ */
#ifndef __declspec
#  define __declspec(x)              PS3__DECLSPEC_##x
#  define PS3__DECLSPEC_thread       __thread
#  define PS3__DECLSPEC_noinline     __attribute__((noinline))
#  define PS3__DECLSPEC_noreturn     __attribute__((noreturn))
#  define PS3__DECLSPEC_deprecated   __attribute__((deprecated))
#  define PS3__DECLSPEC_selectany    __attribute__((weak))
#  define PS3__DECLSPEC_dllimport
#  define PS3__DECLSPEC_dllexport    __attribute__((visibility("default")))
#  define PS3__DECLSPEC_restrict
#  define PS3__DECLSPEC_align(n)     __attribute__((aligned(n)))
#endif
#ifndef __forceinline
#  define __forceinline inline __attribute__((always_inline))
#endif
#ifndef __cdecl
#  define __cdecl
#endif
#ifndef __stdcall
#  define __stdcall
#endif
#ifndef __fastcall
#  define __fastcall
#endif
#ifndef __vectorcall
#  define __vectorcall
#endif
#ifndef __int8
#  define __int8  char
#  define __int16 short
#  define __int32 int
#  define __int64 long long
#endif
#ifndef _MAX_PATH
#  ifdef PATH_MAX
#    define _MAX_PATH PATH_MAX
#  else
#    define _MAX_PATH 4096
#  endif
#endif
#ifndef _countof
#  define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* --- intrinsics ------------------------------------------------------------ */
#define _ReturnAddress()           __builtin_return_address(0)
#define _AddressOfReturnAddress()  __builtin_frame_address(0)
#define __debugbreak()             __builtin_trap()
#define __assume(x)                do { if (!(x)) __builtin_unreachable(); } while (0)
#define __noop(...)                ((void)0)

static inline unsigned short     _byteswap_ushort(unsigned short v)      { return __builtin_bswap16(v); }
static inline uint32_t           _byteswap_ulong(uint32_t v)             { return __builtin_bswap32(v); }
static inline unsigned long long _byteswap_uint64(unsigned long long v)  { return __builtin_bswap64(v); }

static inline unsigned char _BitScanForward(uint32_t* index, uint32_t mask)
{ if (!mask) return 0; *index = (uint32_t)__builtin_ctz(mask); return 1; }
static inline unsigned char _BitScanReverse(uint32_t* index, uint32_t mask)
{ if (!mask) return 0; *index = 31u - (uint32_t)__builtin_clz(mask); return 1; }
static inline unsigned char _BitScanForward64(uint32_t* index, uint64_t mask)
{ if (!mask) return 0; *index = (uint32_t)__builtin_ctzll(mask); return 1; }
static inline unsigned char _BitScanReverse64(uint32_t* index, uint64_t mask)
{ if (!mask) return 0; *index = 63u - (uint32_t)__builtin_clzll(mask); return 1; }

static inline unsigned int       __popcnt(uint32_t v)            { return (unsigned int)__builtin_popcount(v); }
static inline unsigned long long __popcnt64(unsigned long long v){ return (unsigned long long)__builtin_popcountll(v); }
static inline unsigned int       __lzcnt(uint32_t v)             { return v ? (unsigned int)__builtin_clz(v) : 32u; }
static inline unsigned int       __lzcnt64(unsigned long long v) { return v ? (unsigned int)__builtin_clzll(v) : 64u; }
static inline unsigned int       _tzcnt_u32(uint32_t v)          { return v ? (unsigned int)__builtin_ctz(v) : 32u; }
static inline unsigned long long _tzcnt_u64(unsigned long long v){ return v ? (unsigned long long)__builtin_ctzll(v) : 64u; }

static inline uint32_t _rotl(uint32_t v, int s)               { s &= 31; return s ? (v << s) | (v >> (32 - s)) : v; }
static inline uint32_t _rotr(uint32_t v, int s)               { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
static inline unsigned long long _rotl64(unsigned long long v, int s) { s &= 63; return s ? (v << s) | (v >> (64 - s)) : v; }
static inline unsigned long long _rotr64(unsigned long long v, int s) { s &= 63; return s ? (v >> s) | (v << (64 - s)) : v; }

static inline unsigned long long __umulh(unsigned long long a, unsigned long long b)
{ return (unsigned long long)(((unsigned __int128)a * b) >> 64); }
static inline unsigned long long _umul128(unsigned long long a, unsigned long long b, unsigned long long* hi)
{ unsigned __int128 p = (unsigned __int128)a * b; *hi = (unsigned long long)(p >> 64); return (unsigned long long)p; }
static inline long long __mulh(long long a, long long b)
{ return (long long)(((__int128)a * b) >> 64); }

/* --- CRT: strings ---------------------------------------------------------- */
#define _stricmp    strcasecmp
#define _strnicmp   strncasecmp
#define stricmp     strcasecmp
#define strnicmp    strncasecmp
#define _strdup     strdup
#define _snprintf   snprintf
#define _vsnprintf  vsnprintf
#define _snprintf_s(buf, size, count, ...)   snprintf((buf), (size), __VA_ARGS__)
#define _vsnprintf_s(buf, size, count, fmt, ap) vsnprintf((buf), (size), (fmt), (ap))
#define sprintf_s(buf, size, ...)            snprintf((buf), (size), __VA_ARGS__)
#define vsprintf_s(buf, size, fmt, ap)       vsnprintf((buf), (size), (fmt), (ap))

typedef int errno_t;
static inline errno_t strcpy_s(char* dst, size_t size, const char* src)
{
    if (!dst || size == 0) return EINVAL;
    if (!src) { dst[0] = '\0'; return EINVAL; }
    size_t n = strlen(src);
    if (n + 1 > size) { dst[0] = '\0'; return ERANGE; }
    memcpy(dst, src, n + 1);
    return 0;
}
static inline errno_t strncpy_s(char* dst, size_t size, const char* src, size_t count)
{
    if (!dst || size == 0) return EINVAL;
    if (!src) { dst[0] = '\0'; return EINVAL; }
    size_t n = strnlen(src, count);
    if (n + 1 > size) { dst[0] = '\0'; return ERANGE; }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return 0;
}
static inline errno_t strcat_s(char* dst, size_t size, const char* src)
{
    if (!dst || size == 0 || !src) return EINVAL;
    size_t d = strnlen(dst, size);
    if (d == size) return EINVAL;
    return strcpy_s(dst + d, size - d, src);
}
static inline char* strtok_s(char* s, const char* delim, char** ctx) { return strtok_r(s, delim, ctx); }

/* --- CRT: files and directories ------------------------------------------- */
static inline errno_t fopen_s(FILE** f, const char* name, const char* mode)
{
    if (!f) return EINVAL;
    *f = fopen(name, mode);
    return *f ? 0 : (errno ? errno : EINVAL);
}
#define _fseeki64   fseeko
#define _ftelli64   ftello
#define _fileno     fileno
#define _mkdir(p)   mkdir((p), 0777)
#define _rmdir      rmdir
#define _unlink     unlink
#define _access     access
#define _getcwd     getcwd
#define _chdir      chdir
#define _stat       stat          /* both the struct tag and the function */
#define _stat64     stat
#define _fstat      fstat
#define _fstat64    fstat
#define _S_IFDIR    S_IFDIR
#define _S_IFREG    S_IFREG

/* --- CRT: environment and time --------------------------------------------- */
static inline errno_t _putenv_s(const char* name, const char* value)
{ return setenv(name, value ? value : "", 1) == 0 ? 0 : errno; }
static inline int _putenv(char* s) { return putenv(s); }
static inline errno_t localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : EINVAL; }
static inline errno_t gmtime_s(struct tm* out, const time_t* t)    { return gmtime_r(t, out) ? 0 : EINVAL; }

/* --- CRT: memory ----------------------------------------------------------- */
static inline void* _aligned_malloc(size_t size, size_t align)
{
    void* p = NULL;
    if (align < sizeof(void*)) align = sizeof(void*);
    return posix_memalign(&p, align, size ? size : 1) == 0 ? p : NULL;
}
#define _aligned_free  free
#define _alloca        alloca

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* !_MSC_VER */
#endif /* PS3RECOMP_MSVC_COMPAT_H */

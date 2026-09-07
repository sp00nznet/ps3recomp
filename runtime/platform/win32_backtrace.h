/*
 * ps3recomp - Win32 host-backtrace API on POSIX
 *
 * The PPU boot scaffold is dense with host-side backtrace diagnostics: when a
 * guest store lands somewhere impossible, or an OPD resolves to nothing, the
 * only useful question is which lifted function did it, and the answer is the
 * host call stack at that moment. Those probes are written against
 * RtlCaptureStackBackTrace and GetModuleHandleA, and they account for the
 * single largest block of Windows-only symbols in runtime/ppu/ -- around sixty
 * uses, none of them load-bearing for actually running a game.
 *
 * Fencing every one off behind #ifdef _WIN32 would compile, and would also
 * mean the platform being brought up is the one platform with no way to
 * diagnose it. So, following runtime/platform/win32_compat.h: supply the names
 * on POSIX and let the call sites stand.
 *
 * The mapping is exact enough for what the probes print, which is always an
 * RVA -- a frame address minus the image base -- to be fed back through
 * llvm-symbolizer or addr2line:
 *
 *   RtlCaptureStackBackTrace -> backtrace(3)
 *   GetModuleHandleA(NULL)   -> dladdr(3) on this image, giving its load base
 *   GetModuleHandleExA(..FROM_ADDRESS..) -> dladdr(3) on the queried address
 *   GetModuleFileNameA       -> the path dladdr(3) reports for that image
 *
 * A returned HMODULE is therefore a load base, not a Win32 module handle, and
 * is only ever valid to subtract. That is all the scaffold does with it.
 *
 * The W forms of those four are here as well, resolving through exactly the
 * same calls. A runner reaches for them not because it has wide strings but
 * because the W name is the one Win32 documents; the only place the character
 * type matters at all is GetModuleFileNameW's output, and the note on it says
 * what that conversion does and does not promise.
 *
 * The dbghelp names -- SymInitialize, SymFromAddr, SymCleanup -- are here too,
 * over the same dladdr, because a crash report that prints a raw RVA is worth
 * much less than one that prints a name. What dladdr can and cannot answer is
 * not the same thing dbghelp answers off a PDB, and the difference is worth
 * being exact about:
 *
 *   macOS: dyld resolves from the image's own symbol table, which for an
 *   unstripped build carries local symbols as well, so a file-static function
 *   usually does resolve. Strip the binary and nothing does.
 *
 *   Linux: dladdr sees the DYNAMIC symbol table only. A function is resolvable
 *   if it is exported -- which for an executable means linking -rdynamic
 *   (-Wl,--export-dynamic); without it SymFromAddr succeeds for library
 *   functions and fails for the program's own. A `static` function is never in
 *   that table and never resolves.
 *
 *   Neither: no line numbers, no inlined frames, no symbol size (SYMBOL_INFO's
 *   Size reads zero), and no way to load symbols from a separate file the way
 *   SymInitialize loads a PDB. SymInitialize therefore has nothing to do and
 *   succeeds; the name comes from whatever the loaded image already carries.
 *
 * Kept out of win32_compat.h deliberately. That header is included by library
 * translation units (cellSpurs.c, cellGcmSys.c, spu_channels.c, lv2_register.c)
 * and <execinfo.h> does not exist on musl, so folding these in would put a
 * needless portability floor under the whole runtime for the sake of a
 * debug-only facility the library never calls.
 */
#ifndef PS3RECOMP_WIN32_BACKTRACE_H
#define PS3RECOMP_WIN32_BACKTRACE_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#else /* !_WIN32 */

#include <stddef.h>
#include <stdint.h>   /* uintptr_t, for the function-pointer -> void* cast */
#include <string.h>   /* the symbol and module names are copied out */

/* backtrace(3) is a glibc/Apple extension; musl ships no <execinfo.h>. Where it
 * is missing the shims stay, but report an empty stack -- the diagnostics then
 * print nothing useful instead of failing to build, which is the right trade
 * for a debug path. */
#if defined(__GLIBC__) || defined(__APPLE__)
#  include <execinfo.h>
#  define PS3_HAVE_BACKTRACE 1
#else
#  define PS3_HAVE_BACKTRACE 0
#endif
#include <dlfcn.h>
/* glibc declares Dl_info and dladdr only under _GNU_SOURCE, which must be
 * defined before the translation unit's first system header. C++ compilers
 * on Linux define it themselves; a C translation unit has to, and finding
 * out through "unknown type name Dl_info" is worse than being told. */
#if defined(__GLIBC__) && !defined(__USE_GNU)
#  error "win32_backtrace.h needs _GNU_SOURCE defined before the first #include on glibc (dladdr's Dl_info)"
#endif

#ifndef PS3_WIN32_TYPES_USHORT
#define PS3_WIN32_TYPES_USHORT
typedef unsigned short USHORT;
#endif
typedef void*       HMODULE;
/* Spelt exactly as win32_compat.h spells them, so a translation unit that
 * includes both is redefining each typedef to the same type, which C11 and
 * C++11 both allow. This header stays usable on its own, which is how
 * ppu_hle.cpp and ppu_fs.cpp include it. */
typedef const char* LPCSTR;
typedef const char* PCSTR;
typedef char*       LPSTR;
typedef char        CHAR;
typedef wchar_t*       LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef int         BOOL;
typedef uint32_t    DWORD;
typedef uint32_t    ULONG;
typedef unsigned long long ULONG64;
typedef unsigned long long DWORD64;
typedef void*       HANDLE;

/* Only the two flags the scaffold passes. Both are honoured implicitly:
 * dladdr always resolves from an address and never takes a reference. */
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS       0x00000004

/* Load base of the mapped object containing `addr`, or NULL. */
static inline void* ps3__image_base_of(const void* addr)
{
    Dl_info info;
    if (addr && dladdr(addr, &info) && info.dli_fbase)
        return info.dli_fbase;
    return NULL;
}

/* GetModuleHandleA(NULL) means "the image I am running in". The scaffold never
 * asks for any other module, so a named lookup would be dead code; resolving
 * this header's own address gives the right answer and needs no name. */
static inline HMODULE GetModuleHandleA(LPCSTR name)
{
    (void)name;
    return (HMODULE)ps3__image_base_of((const void*)(uintptr_t)&ps3__image_base_of);
}

static inline int GetModuleHandleExA(unsigned long flags, LPCSTR addr,
                                     HMODULE* out)
{
    (void)flags;
    if (!out) return 0;
    *out = (HMODULE)ps3__image_base_of((const void*)addr);
    return *out != NULL;   /* nonzero == success, as on Win32 */
}

/* The W forms answer the same questions. Nothing here reads the name argument,
 * for the reason the A form does not, and the address the Ex form is given is
 * an address whichever character type it was declared with -- a runner spells
 * it LPCWSTR because that is the parameter Win32 declares, not because there
 * is a string at the other end. */
static inline HMODULE GetModuleHandleW(LPCWSTR name)
{
    (void)name;
    return (HMODULE)ps3__image_base_of((const void*)(uintptr_t)&ps3__image_base_of);
}

static inline int GetModuleHandleExW(unsigned long flags, LPCWSTR addr,
                                     HMODULE* out)
{
    (void)flags;
    if (!out) return 0;
    *out = (HMODULE)ps3__image_base_of((const void*)addr);
    return *out != NULL;
}

/* Win32 counts `skip` from the CALLER of RtlCaptureStackBackTrace; backtrace(3)
 * starts at itself, so one extra frame comes off the top for this shim. */
static inline USHORT RtlCaptureStackBackTrace(unsigned long skip,
                                             unsigned long count,
                                             void** frames,
                                             unsigned long* hash)
{
    if (hash) *hash = 0;
    if (!frames || count == 0) return 0;
#if PS3_HAVE_BACKTRACE
    enum { PS3_BT_CAP = 160 };   /* deepest in-tree request is 62 + skip */
    void* raw[PS3_BT_CAP];
    unsigned long want = skip + count + 1;   /* +1: this frame */
    if (want > PS3_BT_CAP) want = PS3_BT_CAP;

    int got = backtrace(raw, (int)want);
    if (got <= 0) return 0;

    unsigned long first = skip + 1;
    if (first >= (unsigned long)got) return 0;

    unsigned long n = (unsigned long)got - first;
    if (n > count) n = count;
    for (unsigned long i = 0; i < n; i++) frames[i] = raw[first + i];
    return (USHORT)n;
#else
    (void)skip; (void)count;
    return 0;
#endif
}

/* ---------------------------------------------------------------------------
 * The dbghelp symbol names
 *
 * SYMBOL_INFO keeps dbghelp's field order: callers size a raw buffer as
 * sizeof(SYMBOL_INFO) + N, set MaxNameLen to N - 1 and read Name back out, so
 * the trailing Name[1] and the fields before it have to sit where dbghelp puts
 * them. SizeOfStruct is accepted and ignored; there is only one version here.
 * -----------------------------------------------------------------------*/
typedef struct {
    ULONG   SizeOfStruct;
    ULONG   TypeIndex;
    ULONG64 Reserved[2];
    ULONG   Index;
    ULONG   Size;             /* always 0: dladdr has no symbol extent */
    ULONG64 ModBase;
    ULONG   Flags;
    ULONG64 Value;
    ULONG64 Address;
    ULONG   Register;
    ULONG   Scope;
    ULONG   Tag;
    ULONG   NameLen;
    ULONG   MaxNameLen;
    CHAR    Name[1];
} SYMBOL_INFO;
typedef SYMBOL_INFO* PSYMBOL_INFO;

/* Nothing to load and nothing to release: the names come from the images the
 * loader already mapped. Both succeed so a caller's error path stays dead. */
static inline BOOL SymInitialize(HANDLE process, PCSTR search_path, BOOL invade)
{ (void)process; (void)search_path; (void)invade; return 1; }
static inline BOOL SymCleanup(HANDLE process) { (void)process; return 1; }

static inline BOOL SymFromAddr(HANDLE process, DWORD64 address,
                               DWORD64* displacement, PSYMBOL_INFO symbol)
{
    (void)process;
    if (displacement) *displacement = 0;
    if (!symbol) return 0;

    Dl_info info;
    if (!dladdr((const void*)(uintptr_t)address, &info) || !info.dli_sname || !info.dli_saddr)
        return 0;

    if (displacement)
        *displacement = (DWORD64)((uintptr_t)address - (uintptr_t)info.dli_saddr);
    symbol->Address = (ULONG64)(uintptr_t)info.dli_saddr;
    symbol->Value   = symbol->Address;
    symbol->ModBase = (ULONG64)(uintptr_t)info.dli_fbase;
    symbol->Size    = 0;
    symbol->Flags   = 0;

    ULONG cap = symbol->MaxNameLen ? symbol->MaxNameLen : 1u;
    size_t n = strlen(info.dli_sname);
    if (n > (size_t)(cap - 1)) n = (size_t)(cap - 1);
    memcpy(symbol->Name, info.dli_sname, n);
    symbol->Name[n] = '\0';
    symbol->NameLen = (ULONG)n;
    return 1;
}

/* The path of the mapped object, from the loader's own record of it. NULL asks
 * for the image this code is in. A name too long for the buffer is truncated
 * rather than refused, and the returned length is what was written. */
static inline DWORD GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size)
{
    if (!buffer || size == 0) return 0;
    buffer[0] = '\0';
    const void* probe = module ? (const void*)module
                              : (const void*)(uintptr_t)&ps3__image_base_of;
    Dl_info info;
    if (!dladdr(probe, &info) || !info.dli_fname) return 0;
    size_t n = strlen(info.dli_fname);
    if (n > (size_t)(size - 1)) n = (size_t)(size - 1);
    memcpy(buffer, info.dli_fname, n);
    buffer[n] = '\0';
    return (DWORD)n;
}

/* Widened one byte to one wchar_t, which is the simple conversion and not the
 * correct one: a UTF-8 path arrives as one wide character per BYTE, so a
 * non-ASCII component prints as mojibake. Doing it properly means mbstowcs
 * against a locale the process has usually not set, and the whole audience for
 * this string is a crash-time "which module was that" line where a build path
 * is ASCII in practice. Truncation matches the A form: what fits is written,
 * terminated, and its length returned. */
static inline DWORD GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size)
{
    if (!buffer || size == 0) return 0;
    buffer[0] = L'\0';
    const void* probe = module ? (const void*)module
                              : (const void*)(uintptr_t)&ps3__image_base_of;
    Dl_info info;
    if (!dladdr(probe, &info) || !info.dli_fname) return 0;
    size_t n = strlen(info.dli_fname);
    if (n > (size_t)(size - 1)) n = (size_t)(size - 1);
    for (size_t i = 0; i < n; i++)
        buffer[i] = (wchar_t)(unsigned char)info.dli_fname[i];
    buffer[n] = L'\0';
    return (DWORD)n;
}

#endif /* _WIN32 */
#endif /* PS3RECOMP_WIN32_BACKTRACE_H */

/*
 * ps3recomp - guest write watch (default off: PS3RECOMP_VM_WRITE_WATCH=1)
 *
 * Lets a consumer that keeps a copy of guest memory (for example a renderer
 * retaining unchanged asset data between frames) learn that the memory has not
 * been written since it last looked, without reading it again.
 *
 * vm_watch() write-protects the host pages covering a guest range and returns
 * an epoch. A later write to such a page by any user-space code (PPU, SPU DMA,
 * runtime/HLE copies) faults once; the fault handler records a new epoch for
 * the page, makes it writable again and lets the write proceed. So
 * vm_clean_since(range, epoch) is true only while no page of the range has
 * been written (or had its protection changed by the runtime) since that
 * watch.
 *
 * A kernel write (read(2)/fread into guest memory, recv, ReadFile, ...) does
 * not fault: it fails with EFAULT/ERROR_NOACCESS on a protected page. Every
 * such caller announces the write first with vm_note_write() (vm_watch_hooks.h); with
 * PS3RECOMP_VM_WRITE_WATCH_STRICT=1 vm_note_write_check() turns a missed one
 * into a fatal report.
 *
 * The OS writers into guest memory, each announced (keep this list current):
 *   runtime/syscalls/sys_fs.c      sys_fs read (fread)
 *   runtime/ppu/ppu_fs.cpp         cellFsRead, cellFsReadWithOffset (fread)
 *   libs/filesystem/cellFs.c       cellFsRead (fread)
 *   libs/network/sysNet.c          recv, recvfrom
 *   libs/network/cellHttp.c        response body recv
 *   libs/system/sysPrxForUser.c    random bytes (/dev/urandom fread)
 *   libs/system/cellSaveData.c     file read into the title's buffer (fread)
 *   libs/network/cellSsl.c         random bytes (/dev/urandom read)
 * Runtime protection and commit changes (vm_commit, vm_protect, vm_shutdown,
 * and the POSIX VirtualAlloc/VirtualFree/VirtualProtect in win32_compat.c)
 * release the pages they touch through vm_watch_hooks.h, and the Windows
 * PPU write guard in ppu_loader.cpp pins its page. No GPU or DMA engine
 * maps guest memory directly; their results reach it through CPU copies, which
 * fault like any other user-space write.
 *
 * The watch works in host pages (16 KB on Apple Silicon, 4 KB elsewhere):
 * a write anywhere in a host page dirties the whole page. Pages the runtime
 * itself made non-writable (vm_protect) are never watched.
 *
 * Rule for SDK code that changes the protection of guest pages itself (for
 * example a diagnostic write guard that mprotects a page and single-steps
 * writes through it): call vm_watch_forget_host(page, size, 0) (or
 * vm_watch_forget(ea, size, 0)) from vm_watch_hooks.h before its first
 * protection change. That pins the pages: the watch releases them if it held
 * them and refuses to arm them from then on (counted in `refused`), so the
 * two never fight over one page's protection. A pin lasts until the runtime
 * makes the pages writable again through vm_commit or vm_protect.
 */
#ifndef VM_WATCH_H
#define VM_WATCH_H

#include <stddef.h>
#include <stdint.h>

#include "vm_watch_hooks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the watch on first use when PS3RECOMP_VM_WRITE_WATCH=1. Returns 1
 * when the watch is active. */
int vm_watch_enable(void);
int vm_watch_active(void);

/* Write-protects the host pages covering [ea, ea+len) and returns the epoch
 * of this watch, or 0 when the watch is inactive or refuses the range (a page
 * the runtime made non-writable, or a protection change failed). Read the
 * range's content after this returns: a write before the protection took
 * effect is in that content, and every write after it is recorded. */
uint64_t vm_watch(uint32_t ea, uint32_t len);

/* 1 when every page of [ea, ea+len) is still watched and has not been written
 * since `epoch` (a value vm_watch returned). */
int vm_clean_since(uint32_t ea, uint32_t len, uint64_t epoch);

/* Announcing OS writes (vm_note_write, vm_note_host_write,
 * vm_note_write_check) lives in vm_watch_hooks.h, so writers need no link
 * dependency on this module. */

typedef struct {
    uint64_t watches;     /* vm_watch calls that protected at least one page */
    uint64_t protected_pages;
    uint64_t faults;      /* writes caught on watched pages */
    uint64_t notes;       /* pages released by vm_note_write */
    uint64_t forgotten;   /* pages released by runtime protection changes */
    uint64_t refused;     /* vm_watch calls refused: a page under an SDK-owned
                           * protection (pinned) */
} vm_watch_stats_t;
void vm_watch_stats(vm_watch_stats_t* out);

/* Preservation (copy on first write), for a consumer that reads guest memory
 * after it was released to the guest but needs it as of one moment:
 *
 *   int s = vm_preserve_begin();          // 0: unavailable
 *   vm_watch_preserve(ea, len) ...        // arm what it will read
 *   uint64_t e = vm_preserve_seal(s);     // the moment
 *   ... the guest runs and writes ...
 *   vm_preserve_read(s, ea, dst, len, &pages);  // bytes as of e
 *   vm_preserve_end(s);
 *
 * A write to a page armed with vm_watch_preserve copies the page's bytes into
 * a preallocated slot before the page becomes writable (in the fault handler:
 * no allocation), when a sealed session needs them. vm_preserve_read takes a
 * page from its slot, or live while it is still protected since before e.
 * It returns -1 when some page could not be had as of e (never armed, armed
 * after e, or released with no free slot); dst then holds live bytes for those
 * pages and the caller must check them itself. Kernel writes (vm_note_write)
 * and runtime protection changes preserve the same way first.
 *
 * Pool: PS3RECOMP_VM_PRESERVE_POOL_MB (default 32), allocated by the first
 * vm_preserve_begin. At most 4 sessions are open at once. */
#define PS3RECOMP_VM_PRESERVE_API 1
uint64_t vm_watch_preserve(uint32_t ea, uint32_t len);
int vm_preserve_begin(void);
uint64_t vm_preserve_seal(int session);
int vm_preserve_read(int session, uint32_t ea, void* dst, uint32_t len,
                     uint32_t* preserved_pages);
void vm_preserve_end(int session);

typedef struct {
    uint64_t sessions;         /* sessions begun */
    uint64_t refused_sessions; /* none free */
    uint64_t preserved_pages;  /* pages copied before a write */
    uint64_t lost_pages;       /* needed, but no free slot */
    uint64_t read_preserved;   /* page reads answered from a slot */
    uint64_t read_live;        /* page reads answered live (still protected) */
    uint64_t read_unpreserved; /* page reads with no copy as of the session */
} vm_preserve_stats_t;
void vm_preserve_stats(vm_preserve_stats_t* out);

#ifdef __cplusplus
}
#endif

#endif /* VM_WATCH_H */

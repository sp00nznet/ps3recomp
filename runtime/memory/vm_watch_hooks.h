/* Optional guest write-watch hooks (runtime/memory/vm_watch.h).
 *
 * Everything here is inert until the watch is enabled: each call is one load
 * of a null pointer. Code that only announces writes or protection changes
 * includes this header and needs nothing else at link time; vm_watch.c fills
 * the table when PS3RECOMP_VM_WRITE_WATCH=1 enables the watch.
 *
 * A protection change the runtime makes itself (vm_commit, vm_protect, or any
 * SDK code that mprotects/VirtualProtects a guest page directly) takes the
 * affected pages out of the write watch first, so the watch never restores or
 * keeps a protection the runtime did not ask for. `writable == 0` pins the
 * pages: never watched until made writable again.
 *
 * A kernel write into guest memory (fread, recv, ...) cannot be caught by a
 * fault; it fails with EFAULT on a protected page. Such a caller announces the
 * write first with vm_note_write()/vm_note_host_write(), and reports an access
 * failure afterwards with vm_note_write_check(). */
#ifndef PS3_VM_WATCH_HOOKS_H
#define PS3_VM_WATCH_HOOKS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    /* [addr, addr+size) is about to get protection `writable` (nonzero:
     * read-write) from the runtime. */
    void (*forget)(uint32_t addr, uint32_t size, int writable);
    /* The same for a host range (ignored outside guest memory): host-level
     * protection or commit calls (VirtualAlloc/VirtualProtect/VirtualFree and
     * their POSIX equivalents) on guest pages. */
    void (*forget_host)(const void* host, size_t size, int writable);
    /* An OS write into [ea, ea+len) is about to happen. */
    void (*note_write)(uint32_t ea, size_t len);
    /* The same, for a host pointer into guest memory (ignored outside it). */
    void (*note_host_write)(const void* host, size_t len);
    /* After an announced OS write: `failed_access` is nonzero when it failed
     * with an access error. Fatal in strict mode, naming `site`. */
    void (*note_write_check)(int failed_access, const char* site);
} ps3_vm_watch_hooks;
#ifdef _MSC_VER
__declspec(selectany) ps3_vm_watch_hooks ps3_vm_watch_hook = {0};
#else
__attribute__((weak)) ps3_vm_watch_hooks ps3_vm_watch_hook = {0};
#endif
static inline void vm_watch_forget(uint32_t addr, uint32_t size, int writable)
{
    if (ps3_vm_watch_hook.forget) ps3_vm_watch_hook.forget(addr, size, writable);
}
static inline void vm_watch_forget_host(const void* host, size_t size, int writable)
{
    if (ps3_vm_watch_hook.forget_host) ps3_vm_watch_hook.forget_host(host, size, writable);
}
static inline void vm_note_write(uint32_t ea, size_t len)
{
    if (ps3_vm_watch_hook.note_write) ps3_vm_watch_hook.note_write(ea, len);
}
static inline void vm_note_host_write(const void* host, size_t len)
{
    if (ps3_vm_watch_hook.note_host_write) ps3_vm_watch_hook.note_host_write(host, len);
}
static inline void vm_note_write_check(int failed_access, const char* site)
{
    if (failed_access && ps3_vm_watch_hook.note_write_check)
        ps3_vm_watch_hook.note_write_check(failed_access, site);
}
#ifdef __cplusplus
}
#endif
#endif

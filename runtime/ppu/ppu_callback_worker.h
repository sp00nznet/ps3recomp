#ifndef PPU_CALLBACK_WORKER_H
#define PPU_CALLBACK_WORKER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* All functions return 0 on success, -1 on failure. Enter/leave/call belong
 * to the SAME host thread. No guest execution occurs in resolve or enter.
 * Enter requires an initialized guest thread registry and stack allocator.
 * Explicit TLS metadata must describe the immutable ELF initialization image,
 * never another thread's live TLS. Allocations are retained until VM teardown.
 */
int ppu_callback_worker_enter(void);
int ppu_callback_worker_enter_with_tls(uint32_t image, uint32_t filesz, uint32_t memsz);
int ppu_callback_worker_leave(void);
int ppu_callback_worker_resolve(uint32_t opd, uint32_t* code, uint32_t* toc);
int ppu_callback_worker_call(uint32_t opd, const uint64_t args[8], uint64_t* result);
int ppu_callback_worker_call_resolved(uint32_t code, uint32_t toc,
                                      const uint64_t args[8], uint64_t* result);
struct ppu_context;
/* Synchronous callback on the actual active guest caller. Caller must be the
 * same host thread's g_active_ctx; borrows its stack/TLS/identity and restores
 * its registers/trampoline, including on C++ exceptions. */
int ppu_callback_call_current(struct ppu_context* caller, uint32_t code, uint32_t toc,
                              const uint64_t args[8], uint64_t* result);
/* Close the entry gate. Existing workers may drain accepted callbacks. A zero
 * result proves every worker has left; only THEN may the owner release VM.
 * The VM owner must obey this protocol; arbitrary concurrent unmaps are not
 * made safe. Resume is allowed only with no workers and a fully initialized VM.
 */
int ppu_callback_workers_quiesce(void);
int ppu_callback_workers_resume(void);
#ifdef __cplusplus
}
#endif
#endif

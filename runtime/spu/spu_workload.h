/* spu_workload.h — SPU workload / task dispatch registry.
 *
 * The bridge cellSpurs needs: when a title registers a SPURS workload
 * (cellSpursAddWorkload, a "policy module" SPU binary) or a SPURS task
 * (cellSpursCreateTask, a task ELF), the runtime must actually RUN that SPU
 * program. We do not interpret SPU ELFs at runtime — we statically lift each
 * title's SPU binaries ahead of time (spu_lifter -> a native entry fn). This
 * layer maps a registered SPU image, by content FINGERPRINT, to its pre-lifted
 * native entry, loads the image into a fresh 256 KB local store, and runs it
 * with the SPURS task ABI (job/task arg EA in r3) via the lifted-job adapter.
 *
 * Flow:
 *   1) At init, the title's generated registration code calls
 *      spu_workload_register(fingerprint, lifted_entry, "name") once per lifted
 *      SPU binary. The fingerprint is FNV-1a-64 over the exact ELF image bytes
 *      (the same bytes the title later hands to cellSpurs), computed identically
 *      here and by the offline extractor (so a manifest can be emitted).
 *   2) cellSpursAddWorkload / cellSpursCreateTask call spu_workload_dispatch()
 *      with the guest image -> fingerprint -> lifted entry -> load LS -> run.
 *
 * This is title-agnostic ps3recomp runtime: the registry is populated by the
 * title's lifted SPU set; the dispatch mechanism is generic.
 */
#ifndef SPU_WORKLOAD_H
#define SPU_WORKLOAD_H

#include "spu_context.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spu_lifted_entry_fn)(spu_context*);

/* FNV-1a 64-bit over a byte range. The canonical workload fingerprint; the
 * offline extractor computes the same value so registrations and the images a
 * title passes to cellSpurs line up. */
uint64_t spu_workload_fingerprint(const void* data, size_t n);

/* Register one lifted SPU binary under its image fingerprint. `name` is borrowed
 * (kept by pointer for logging — pass a string literal / static). Idempotent on
 * fingerprint: a second register of the same fp updates the entry. */
void spu_workload_register(uint64_t fingerprint, spu_lifted_entry_fn fn,
                           const char* name);

/* As spu_workload_register, but binds the image's indirect-branch table id
 * (the value passed to spu_begin_image() before this image's recomp_register).
 * dispatch sets the run context's image_id to this so spu_indirect_branch
 * resolves within the correct image when several share LS addresses. */
void spu_workload_register_img(uint64_t fingerprint, spu_lifted_entry_fn fn,
                               int image_id, const char* name);

/* Look up a lifted entry by fingerprint; NULL if none registered. */
spu_lifted_entry_fn spu_workload_find(uint64_t fingerprint);

/* As spu_workload_find, but also returns the image's indirect-branch table id. */
spu_lifted_entry_fn spu_workload_find_img(uint64_t fingerprint, int* image_id_out);

/* Run a SPURS policy module (raw PM binary loaded at LS 0xA00) under the HLE'd
 * kernel handoff: kernel context at LS 0x100, entry ABI of
 * cellSpursModuleEntry(context, ea) with the workload's u64 data in r4.
 * Returns 0 if the module exited back to the kernel, negative otherwise.
 * (spurs_policy.c) */
int spu_run_policy_module(spu_lifted_entry_fn entry, int image_id,
                          const uint8_t* pm_host, uint32_t pm_size,
                          uint64_t wkl_data, uint32_t wid, uint32_t spurs_ea);

/* Stage and enter one SPURS jobchain job (spurs_job.c). Unlike a policy module
 * or a task, a job binary is a raw image built by the SDK's job_elf-to-bin and
 * linked against Sony's job_start_w_crt.o, whose entry contract is
 *   cellSpursJobMain2(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
 * with r3 = ctx, r4 = job and BOTH pointers into local store. This lays out
 * LS the way jm2 does (per
 * _cellSpursCheckJob in the SDK's job_descriptor.h: binary+bss, ioBuffer,
 * oBuffer, unified scratch+stack, cacheBuffer[0..3], all 1024-aligned), fills
 * ioBuffer from the descriptor's input DMA list, builds the context, and runs
 * the lifted entry. `job_desc_size` is the jobchain's sizeJobDescriptor (0 =>
 * assume 256). Returns 0 if the job ran, negative if it could not be staged. */
int spu_run_spurs_job(spu_lifted_entry_fn entry, int image_id,
                      uint32_t job_ea, uint32_t job_desc_size);

/* Fingerprint `image`, look up its lifted entry, and run it as a SPURS jobchain
 * job via spu_run_spurs_job. Runs SYNCHRONOUSLY on the caller's thread: a job
 * is not persistent (it runs to completion and returns to the manager), and a
 * chain's jobs are ordered by its own NEXT/CALL/RET commands, so the jobchain
 * thread is the right place to run them. Returns 1 if a lifted image matched
 * and ran, 0 if none is registered for this binary. */
int spu_workload_dispatch_job(const uint8_t* image, uint32_t image_size,
                              uint32_t job_ea, uint32_t job_desc_size);

/* Load a 32-bit big-endian SPU ELF image into a 256 KB local store: each PT_LOAD
 * segment is copied to its p_vaddr (BSS zero-filled). Returns 1 on success and
 * writes the entry vaddr (e_entry, which may legitimately be 0) to *entry_out;
 * returns 0 if `image` is not a valid SPU ELF or a segment is out of range.
 * `ls` must point to SPU_LS_SIZE bytes (caller-zeroed if a clean BSS is wanted). */
int spu_elf_load_to_ls(const uint8_t* image, size_t image_size, uint8_t* ls,
                       uint32_t* entry_out);

/* Compute the true on-disk size of a 32-bit BE SPU ELF: the extent that spans
 * the section-header table and all program/section content (max p_offset+p_filesz
 * and non-NOBITS sh_offset+sh_size). This matches the offline extractor's sizing,
 * so fingerprints agree. Use it when a caller (e.g. cellSpursCreateTask) is handed
 * an SPU ELF pointer without an explicit length. `max_avail` bounds the read in
 * case the header is corrupt; returns 0 if `image` is not a valid SPU ELF. */
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail);

/* Dispatch a registered SPU image: fingerprint `image`, find its lifted entry,
 * load it into a fresh local store, and run it with `args_ea` in r3 (SPURS task
 * ABI). Returns 1 if an image matched and ran, 0 if no lifted entry is
 * registered for it (caller may fall back to logging/stubbing). */
int spu_workload_dispatch(const uint8_t* image, uint32_t image_size,
                          uint32_t args_ea);

/* Same, but runs the SPU job on its own detached host thread so the PPU caller
 * is not blocked. Required for persistent SPURS service/worker tasks that loop
 * waiting on PPU-side signals (running them inline deadlocks). */
int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size,
                                uint32_t args_ea);

/* Number of currently registered lifted SPU binaries (diagnostics/tests). */
unsigned spu_workload_count(void);

#ifdef __cplusplus
}
#endif

#endif /* SPU_WORKLOAD_H */

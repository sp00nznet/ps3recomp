/* spu_fn_registry.h — lifted-SPU-function registry + self-modification eviction.
 *
 * The registry maps a local-store entry address (for the currently selected
 * image) to its lifted spu_func_*. It is the fast-path side of spu_dispatch:
 * a hit runs compiled code, a miss falls to the interpreter (spu_interp.c).
 *
 * Eviction is the invalidation half of self-modifying-code support. sage's
 * write-watch detects a store/DMA into the SPU code segment and calls
 * spu_invalidate(); any lifted function whose body overlaps the written range
 * is marked stale, so its next lookup MISSES and dispatch interprets the new
 * bytes instead of running the (now wrong) compiled version.
 */
#ifndef SPU_FN_REGISTRY_H
#define SPU_FN_REGISTRY_H

#include <stdint.h>
#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spu_fn)(spu_context*);

/* Select the image subsequent registrations belong to (SPURS kernel/policy/job
 * overlap in LS). Single-image callers leave it 0. */
void spu_begin_image(int image_id);

/* Register one lifted function at its LS entry address. */
void spu_register_function(uint32_t addr, spu_fn fn);

/* Resolve an LS entry address to a lifted function for `image_id` (0 = any).
 * Returns NULL if unregistered OR evicted (self-modified) -> interpret. */
spu_fn spu_lookup(uint32_t addr, int image_id);

/* Mark every lifted function whose body overlaps [lsa, lsa+size) stale. Called
 * by the code-write watcher. Conservative: over-eviction only costs speed (more
 * interpretation), never correctness. Rare (fires on actual code writes). */
void spu_invalidate(uint32_t lsa, uint32_t size);

/* Declare the lifted code segment [lo, hi) so the store/DMA watch knows which
 * writes to treat as potential self-modification. Registration auto-expands the
 * region to include each entry's first word; images with a known .text extent
 * should call this so writes into the LAST function's body are covered too. */
void spu_set_code_region(uint32_t lo, uint32_t hi);

/* Test/diagnostic: how many entries are currently evicted. */
uint32_t spu_registry_evicted_count(void);

#ifdef __cplusplus
}
#endif
#endif /* SPU_FN_REGISTRY_H */

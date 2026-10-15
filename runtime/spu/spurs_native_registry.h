#ifndef PS3RECOMP_SPURS_NATIVE_REGISTRY_H
#define PS3RECOMP_SPURS_NATIVE_REGISTRY_H
#include <stdint.h>
#include "spurs_job_snapshot.h"
#include "spu_context.h"
#ifdef __cplusplus
extern "C" {
#endif
#define SPURS_NATIVE_REGISTRY_LIMIT 16u
#define SPURS_NATIVE_DMA_MAX_STEPS 16u
#define SPURS_NATIVE_DMA_MAX_BYTES 262144u
typedef void (*spurs_native_entry)(spu_context*);
typedef struct spurs_native_layout {uint32_t image,descriptor,context,context_size,stack,stack_size,return_pc;} spurs_native_layout;
enum spurs_native_input_kind {SPURS_NATIVE_INPUT_NONE=0,SPURS_NATIVE_INPUT_EXACT_GET=1,SPURS_NATIVE_INPUT_EXACT_GET_PAIR=2,SPURS_NATIVE_INPUT_ORDERED_DMA=3};
typedef struct spurs_native_input {uint32_t kind,lsa,size,tag;uint64_t ea;uint8_t expected[16];} spurs_native_input;
typedef struct spurs_native_dma_step {uint32_t command,lsa,size,tag;uint64_t ea;const uint8_t *expected;} spurs_native_dma_step;
typedef struct spurs_native_dma_sequence {const spurs_native_dma_step *steps;uint32_t count;} spurs_native_dma_sequence;
typedef struct spurs_native_definition {
 const uint8_t *image,*descriptor_template;uint32_t image_size,descriptor_size;
 spurs_native_entry entry;uint32_t entry_pc;spurs_native_layout layout;spurs_native_input input;
 const char *source_sha256,*build_sha256;
 spurs_native_dma_sequence dma;
} spurs_native_definition;
typedef struct spurs_native_profile {
 const uint8_t *image,*descriptor_template;uint32_t image_size,descriptor_size;uint64_t fingerprint;
 spurs_native_entry entry;uint32_t entry_pc;spurs_native_layout layout;spurs_native_input input;
 char source_sha256[65],build_sha256[65];
 spurs_native_dma_sequence dma;uint32_t allocation_size;
} spurs_native_profile;
typedef enum spurs_native_status {SPURS_NATIVE_OK=0,SPURS_NATIVE_DUPLICATE,
 SPURS_NATIVE_INVALID,SPURS_NATIVE_CONFLICT,SPURS_NATIVE_FULL,SPURS_NATIVE_SEALED,
 SPURS_NATIVE_NOT_SEALED,SPURS_NATIVE_NOT_FOUND,SPURS_NATIVE_NO_MEMORY} spurs_native_status;
/* Trusted native startup only. Copies complete image/template and provenance.
 * Fixed legacy raw contract: BE16+8 image quads and BE16+1C stack quads
 * must agree with the image size and declared stack_size.
 * First8 descriptor bytes are dynamic binaryEA, ignored at registration and
 * canonicalized0 in owned template; bytes8..end match exactly at resolution.
 * SHA256 strings document source/build provenance, not native code verification.
 * NONE requires zero remaining input fields and means caller permits no DMA.
 * EXACT_GET defines exactly one actual GET0x40; caller checks shape and delivered
 * LS bytes after genuine transfer. Registry performs no DMA/OS-readability check.
 * EXACT_GET_PAIR admits either of two distinct complete 1/2/4/8-byte payloads,
 * concatenated in expected; the shape is still exactly one real GET0x40.
 * ORDERED_DMA requires 1..16 ordinary GET0x40/PUT0x20 steps. Its legacy
 * input fields must be zero; legacy inputs require an empty DMA sequence.
 * Each expected pointer supplies stable trusted HOST bytes of that step's
 * size. Registration copies steps and payloads within a 256 KiB budget.
 * GET checks delivered LS bytes; PUT checks LS bytes before the real copy.
 * A failed later step cannot undo earlier real transfers.
 * All unused expected bytes must be zero. EA extent must be representable32bit.
 * allocation_size is checked round-up-to-1024 of requested stack plus the
 * descriptor's BE16+0x1E 16-byte units; stack_size remains the requested size.
 * No title is registered automatically; no context/readiness/dispatch is created.
 * Inputs/output pointers refer to stable caller-owned HOST memory. */
spurs_native_status spurs_native_registry_register(const spurs_native_definition*,const spurs_native_profile **out);
/* Seal succeeds once; repeated calls return SEALED. No unregister/destructor. */
spurs_native_status spurs_native_registry_seal(void);
/* Requires seal and a stable checked snapshot. It validates metadata and exact
 * bytes; FNV is only a prefilter. Returned profile/owned bytes are immutable
 * through this API and live until process exit. Caller must never free/mutate.
 * Resolution does not establish lease ownership or safe LS staging.
 * OK/DUPLICATE set out; every error leaves out and registry unchanged. */
spurs_native_status spurs_native_registry_resolve(const spurs_job_snapshot*,const spurs_native_profile **out);
#ifdef __cplusplus
}
#endif
#endif

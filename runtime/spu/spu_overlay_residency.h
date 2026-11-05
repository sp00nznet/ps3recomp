#ifndef SPU_OVERLAY_RESIDENCY_H
#define SPU_OVERLAY_RESIDENCY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exact code bytes a lifted streamed overlay was generated from, at the
 * local-store addresses the title loads it to. */
typedef struct spu_overlay_code_segment {
    uint32_t lsa, size;
    const uint8_t* bytes;
} spu_overlay_code_segment;

/* Register a streamed code overlay by its DMA source EA, like
 * spu_overlay_register_region, plus the code bytes its lifted functions were
 * generated from. A GET from content_ea into local store at load_lsa marks the
 * overlay resident but unverified; the first dispatch into its span compares
 * the segments once and then dispatches by residency until another DMA lands in
 * the span. A mismatch leaves the span interpreted until the next load.
 * Prefer a span covering the lifted code only, not the whole loaded image:
 * code that DMAs its own data into the image above the code would otherwise
 * clear residency with every such GET, sending later dispatch to the
 * interpreter. The segments are copied. Returns 1 on success, 0 on invalid input or a full
 * registry. */
int spu_overlay_register_verified_region(uint32_t content_ea, uint32_t load_lsa,
    uint32_t span, int image_id, const spu_overlay_code_segment* code,
    unsigned count);

#ifdef __cplusplus
}
#endif

#endif /* SPU_OVERLAY_RESIDENCY_H */

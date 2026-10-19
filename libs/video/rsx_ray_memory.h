#ifndef RSX_RAY_MEMORY_H
#define RSX_RAY_MEMORY_H
#include <stdint.h>
/* Host allocation policy. Device limits and current driver budget, not guest
 * RAM/VRAM sizes. Budget is a changing estimate; Vulkan allocation may still
 * fail and the caller must retain its raster fallback. */
static inline int rsx_ray_memory_fits(uint64_t bytes, uint64_t allocation_limit,
    uint64_t heap_size, int budget_available, uint64_t budget, uint64_t usage)
{
    if (!bytes || !allocation_limit || bytes > allocation_limit || bytes > heap_size)
        return 0;
    if (!budget_available) return 1;
    if (budget > heap_size) budget = heap_size;
    return usage <= budget && bytes <= budget - usage;
}
#endif

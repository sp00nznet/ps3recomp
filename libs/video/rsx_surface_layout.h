#ifndef RSX_SURFACE_LAYOUT_H
#define RSX_SURFACE_LAYOUT_H

/*
 * Header-only RSX surface layout calculation.
 *
 * Computes the checked logical raster extent and the expanded per-sample
 * storage extent for a render target configuration. This helper performs no
 * rasterization, no MSAA resolve and never touches guest state; it only
 * derives dimensions from the supplied register values.
 */

#include <stdint.h>
#include <limits.h>
#include <string.h>

typedef struct rsx_surface_layout {
    uint32_t width, height, storage_width, storage_height, samples, samples_x, samples_y;
} rsx_surface_layout;

/* Maps an AA mode to its sample count and storage grid. Returns 0 if unsupported. */
static inline int rsx_surface_layout_mode_grid(uint32_t aa_mode, uint32_t* samples,
                                               uint32_t* samples_x, uint32_t* samples_y)
{
    switch (aa_mode) {
    case 0: *samples = 1; *samples_x = 1; *samples_y = 1; return 1;
    case 3: *samples = 2; *samples_x = 2; *samples_y = 1; return 1;
    case 4:
    case 5: *samples = 4; *samples_x = 2; *samples_y = 2; return 1;
    default: return 0;
    }
}

/*
 * Clamps *width to the logical capacity of one attachment. The pitch stores
 * every horizontal sample of a row, so a row holds pitch / (bpp * samples_x)
 * logical pixels; this may be smaller than the clip width. The product is
 * formed in 64 bits so a large bpp cannot wrap to a small or zero divisor.
 * Returns 0 if the attachment cannot hold a single logical pixel.
 */
static inline int rsx_surface_layout_clamp_width(uint64_t* width, uint32_t pitch,
                                                 uint32_t bpp, uint32_t samples_x)
{
    uint64_t bytes_per_pixel = (uint64_t)bpp * (uint64_t)samples_x;
    uint64_t capacity = (uint64_t)pitch / bytes_per_pixel;
    if (capacity == 0)
        return 0;
    if (capacity < *width)
        *width = capacity;
    return 1;
}

static inline int rsx_surface_layout_compute(uint32_t aa_mode, uint32_t clip_width,
                                             uint32_t clip_height, uint32_t color_bpp,
                                             const uint32_t color_pitch[4], uint32_t color_mask,
                                             uint32_t depth_bpp, uint32_t depth_pitch,
                                             int depth_used, rsx_surface_layout* out)
{
    uint32_t samples = 0, samples_x = 0, samples_y = 0;
    uint64_t width, storage_width, storage_height;
    uint32_t i;

    if (out == NULL)
        return 0;
    memset(out, 0, sizeof(*out));

    if (clip_width == 0 || clip_height == 0)
        return 0;
    if ((color_mask & ~UINT32_C(0xF)) != 0)
        return 0;
    if (!rsx_surface_layout_mode_grid(aa_mode, &samples, &samples_x, &samples_y))
        return 0;
    if (color_mask == 0 && !depth_used)
        return 0;
    if (color_mask != 0 && (color_pitch == NULL || color_bpp == 0))
        return 0;
    if (depth_used && depth_bpp == 0)
        return 0;

    /*
     * All active attachments share one raster extent, so the logical width is
     * the clip width bounded by the smallest per-attachment capacity. Inactive
     * colour slots and an unused depth buffer do not constrain it.
     */
    width = clip_width;
    for (i = 0; i < 4; ++i) {
        if ((color_mask & (UINT32_C(1) << i)) == 0)
            continue;
        if (!rsx_surface_layout_clamp_width(&width, color_pitch[i], color_bpp, samples_x))
            return 0;
    }
    if (depth_used && !rsx_surface_layout_clamp_width(&width, depth_pitch, depth_bpp, samples_x))
        return 0;

    /* Clip height is already logical; storage expands it by the vertical grid. */
    storage_width = width * (uint64_t)samples_x;
    storage_height = (uint64_t)clip_height * (uint64_t)samples_y;
    if (storage_width > UINT32_MAX || storage_height > UINT32_MAX)
        return 0;

    out->width = (uint32_t)width;
    out->height = clip_height;
    out->storage_width = (uint32_t)storage_width;
    out->storage_height = (uint32_t)storage_height;
    out->samples = samples;
    out->samples_x = samples_x;
    out->samples_y = samples_y;
    return 1;
}

#endif /* RSX_SURFACE_LAYOUT_H */

/* Folklore's final composite at internal raster scales above 1.
 *
 * The title turns its two-sample world into the display image with one
 * full-screen draw that averages two taps of the world texture at
 * (u -/+ f*c8.x, v -/+ f*c9.y), c8.x = 1/1280 and c9.y = 1/720, where f is a
 * depth term clamped below by c7.x = 0.3 and above by 0.85: a resolve with a
 * little blur in focus, a depth-of-field spread for distant pixels. The
 * offsets are guest pixels, so at scale k the in-focus floor still spreads
 * the taps over 0.6 guest = 0.6k host pixels and the whole picture keeps the
 * 720p blur. Dividing only the floor by k gives the in-focus resolve the
 * footprint in host pixels it had in PS3 pixels; the depth-of-field range
 * above it stays the game's.
 *
 * Recognised by the decompiled program's shape and then, per draw, by the
 * three constant values, so another program with the same text is untouched.
 * FOLKLORE_COMPOSITE_SHARP=0 restores the guest floor. */
#ifndef RSX_FOLKLORE_COMPOSITE_H
#define RSX_FOLKLORE_COMPOSITE_H
#include <math.h>
#include <stdint.h>
#include <string.h>

#define RSX_FOLKLORE_COMPOSITE_FLOOR 7u
#define RSX_FOLKLORE_COMPOSITE_DU    8u
#define RSX_FOLKLORE_COMPOSITE_DV    9u

static inline int rsx_folklore_composite_program(const char* ps_hlsl)
{
    return ps_hlsl &&
        strstr(ps_hlsl, "(max(((r[0]).xyzw), ((fp_constants[7]).xxxx)))") &&
        strstr(ps_hlsl, "(((r[0]).xyzw) * ((fp_constants[8]).xxxx))") &&
        strstr(ps_hlsl, "(((r[0]).xxxx) * ((fp_constants[9]).yyyy))") &&
        strstr(ps_hlsl, "rsx_tex[1].Sample") &&
        strstr(ps_hlsl, "rsx_tex[0].Sample(rsx_samp[0], ((r[1]).zwzz).xy)") &&
        strstr(ps_hlsl, "rsx_tex[0].Sample(rsx_samp[0], ((r[0]).zwzz).xy)");
}

/* constants: one draw's packed float4 fragment constants, the composite's ten
 * and the engine's trailing alpha-reference slot. Returns 1 when they are the
 * composite's and the floor was divided by scale, else 0 and the constants
 * are unchanged. */
static inline int rsx_folklore_composite_sharpen(void* constants, uint32_t bytes, float scale)
{
    if (!constants || bytes != (RSX_FOLKLORE_COMPOSITE_DV + 2u) * 16u ||
        !isfinite(scale) || !(scale > 1.0f) || scale > 8.0f) return 0;
    float c[(RSX_FOLKLORE_COMPOSITE_DV + 1u) * 4u];
    memcpy(c, constants, sizeof c);
    const float floor_ = c[RSX_FOLKLORE_COMPOSITE_FLOOR * 4u];
    const float du = c[RSX_FOLKLORE_COMPOSITE_DU * 4u];
    const float dv = c[RSX_FOLKLORE_COMPOSITE_DV * 4u + 1u];
    if (fabsf(floor_ - 0.3f) > 1e-6f || fabsf(du - 1.0f / 1280.0f) > 1e-9f ||
        fabsf(dv - 1.0f / 720.0f) > 1e-8f)
        return 0;
    const float sharpened = floor_ / scale;
    memcpy((unsigned char*)constants + RSX_FOLKLORE_COMPOSITE_FLOOR * 16u, &sharpened, sizeof sharpened);
    return 1;
}
#endif

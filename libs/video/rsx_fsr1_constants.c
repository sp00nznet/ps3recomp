#include "rsx_fsr1_constants.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define A_CPU 1
/* Upstream supports these overrides. Restrict on pointer return types causes
 * ignored-qualifier diagnostics in C; these small setup functions need no
 * aliasing optimization, and unused header helpers can remain inline. */
#define A_RESTRICT
#define A_STATIC static inline
#include "../../third_party/fsr1/ffx_a.h"
#include "../../third_party/fsr1/ffx_fsr1.h"

int rsx_fsr1_make_constants(rsx_fsr1_constants *out,
    uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh, float sharpness)
{
    rsx_fsr1_constants c;
    /* Integers above 2^24 are not exact in the upstream FP32 API. Downscaling
     * uses the renderer's existing filter, not this upscaler. */
    if (!out || !iw || !ih || !ow || !oh || iw > ow || ih > oh ||
        ow > 16777216u || oh > 16777216u ||
        !isfinite(sharpness) || sharpness < 0.f || sharpness > 2.f)
        return -1;
    memset(&c, 0, sizeof(c));
    FsrEasuCon(c.easu[0], c.easu[1], c.easu[2], c.easu[3],
        (float)iw, (float)ih, (float)iw, (float)ih, (float)ow, (float)oh);
    FsrRcasCon(c.rcas, sharpness);
    memcpy(out, &c, sizeof(c));
    return 0;
}

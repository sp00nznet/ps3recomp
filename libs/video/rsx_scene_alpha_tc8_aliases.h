/* Exact aliases for captured alpha contracts after the TC8 input repair.
 * Generated source identities are accepted only after byte-for-byte reversal
 * to the archived full fragment shader and a source-specific alpha lane audit.
 * Keep this list fixed; do not canonicalize arbitrary shader text at runtime. */
#ifndef RSX_SCENE_ALPHA_TC8_ALIASES_H
#define RSX_SCENE_ALPHA_TC8_ALIASES_H
#include <string.h>

static inline const char *rsx_scene_alpha_tc8_original(const char *normalized64hash)
{
    if (!normalized64hash) return NULL;
    /* capture-20261019T012725Z/shaders/pipe33.ps.hlsl */
    if (!strcmp(normalized64hash, "c0dfeb27d8b4564c76a0c4597a2803e2207e45f087352ac278d7435ab6c0a50c"))
        return "816722266a9edefb0d0a70c97b469c5c1d2d65658d8c06ffb296ecc2601ef579";
    /* capture-20261019T012725Z/shaders/pipe36.ps.hlsl */
    if (!strcmp(normalized64hash, "f8edbea31132b75c337c8af49016e831fe10930deaffd151a9317f40216acdc8"))
        return "b2bb97384f7f2e7c066f4560886b0aa2c039ba982cbdb8ae2b824fd289a3b043";
    return normalized64hash;
}
#endif

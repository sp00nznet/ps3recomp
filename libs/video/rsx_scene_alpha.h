/* Captured Folklore alpha contracts, evaluated on interpolated VS outputs.
 * This is a CPU hit-rejection reference, not a generic fragment interpreter. */
#ifndef RSX_SCENE_ALPHA_H
#define RSX_SCENE_ALPHA_H
#include "rsx_scene_bridge.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_scene_alpha_contract rsx_scene_alpha_contract;
typedef enum rsx_scene_alpha_status {
    RSX_SCENE_ALPHA_OK = 0,
    RSX_SCENE_ALPHA_UNSUPPORTED = -1,
    RSX_SCENE_ALPHA_INVALID = -2,
    RSX_SCENE_ALPHA_SAMPLE_UNAVAILABLE = -3,
    RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED = -4
} rsx_scene_alpha_status;

typedef struct rsx_scene_alpha_result {
    float raw_alpha; /* Final guest expression before component NaN sanitation. */
    float alpha;     /* NaN becomes zero; infinity is retained. */
    float reference; /* NAN when the matched shader has no alpha test. */
    int keep;        /* Thresholded: strict alpha > reference. Otherwise always 1. */
    float coverage;  /* Saturated alpha, used only as continuous secondary-ray coverage. */
    int thresholded; /* 1 when the matched shader performs an alpha test. */
} rsx_scene_alpha_result;

/* Return 1 after writing the shader-visible RGBA sample, or 0 if unavailable.
 * The callback must apply the captured descriptor/channel/color-space and
 * sampler/filter/address/mip semantics at the explicitly supplied LOD. It must
 * be independent of call order. Unused sample channels may contain NaNs.
 * No texture decoder, implicit derivative, sampler bias or LOD clamp is guessed
 * by the evaluator. Negative finite LOD is valid and is passed through. */
typedef int (*rsx_scene_alpha_sample_fn)(void* user,unsigned texture_index,
    float u,float v,float explicit_lod,float rgba[4]);

/* Caller must compute SHA256 of the exact captured PS HLSL bytes. Fifteen
 * contracts match. Each has
 * two explicit verified byte identities: captured CRLF and runtime LF source.
 * Both digests are stored; no fuzzy source normalization is performed here.
 * Pipeline numbers alone do not identify supported semantics. Hex is case
 * insensitive; malformed/unknown digests return NULL. No source hashing is
 * performed here. Returned contracts are immutable and safe to share. */
const rsx_scene_alpha_contract* rsx_scene_alpha_contract_for_sha256(const char* sha256_hex);
/* Unknown/NULL contract pointers return NULL, zero, or -1 as appropriate. The SHA accessor returns
 * the canonical captured CRLF identity for either matching source alias.
 * FP size includes the final fp_alpha float4. Its x component is the
 * comparison reference only when the shader actually performs an alpha test. */
const char* rsx_scene_alpha_contract_sha256(const rsx_scene_alpha_contract* contract);
size_t rsx_scene_alpha_required_fp_bytes(const rsx_scene_alpha_contract* contract);
unsigned rsx_scene_alpha_texture_mask(const rsx_scene_alpha_contract* contract);
/* Stable GPU helper IDs: 0..7 are the original strict alpha-test contracts;
 * 8=pipe72 A2C, 9=pipe73 A2C, 10=pipe75 A2C without shader alpha tests;
 * 11=the exact pipe107 strict alpha-test contract;
 * 12=the exact pipe110 A2C contract without a shader alpha test;
 * 13=pipe117 strict alpha; 14=byte-identical pipe119/129 strict Fresnel alpha.
 * IDs may be emitted only after exact source-hash matching.
 * Unknown contracts return -1; never select an ID by pipeline number alone. */
int rsx_scene_alpha_contract_id(const rsx_scene_alpha_contract* contract);
/* Safe reverse lookup for an ID already derived from an exact source hash.
 * Unknown IDs return NULL. */
const rsx_scene_alpha_contract* rsx_scene_alpha_contract_for_id(unsigned id);
/* 1 for a shader alpha test, 0 for no shader alpha test, -1 for unknown. */
int rsx_scene_alpha_contract_has_threshold(const rsx_scene_alpha_contract* contract);

/* Use the full per-draw PS constant buffer with its exact required byte size.
 * Varyings must already have the correct triangle interpolation and projection
 * provenance. Only fields used by the matched scalar alpha contract are read;
 * unused positions/varyings/constants/sample channels may contain NaNs.
 * lod_by_texture provides 16 entries; only dependent texture slots are read.
 *
 * On success, thresholded result.keep=0 rejects this candidate and ray traversal
 * continues; keep=1 accepts it. Without a shader alpha test, keep is always 1;
 * coverage is a clamped scalar proxy, not the exact raster A2C sample mask.
 * Every negative status requires an explicit caller
 * fallback; it MUST NOT be interpreted as alpha rejection or opaque acceptance.
 * Output is unchanged on failure. Sampler callbacks can occur before a failure.
 *
 * Finite normal/fresnel intermediate arithmetic is supported. Undefined zero
 * normalizations, invalid log domains, nonfinite clamp operands or overflow
 * return NUMERIC_UNSUPPORTED rather than guessing GPU NaN/min/max behavior.
 * The final alpha expression follows the captured component NaN sanitation.
 * Host float operations retain expression order, but normalize/log2/exp2 and
 * boundary decisions have not been qualified bit-for-bit against GPU math.
 * Compile without fast-math or contraction; this is an offline CPU reference. */
rsx_scene_alpha_status rsx_scene_alpha_evaluate(const rsx_scene_alpha_contract* contract,
    const rsx_scene_vertex_output* interpolated,const void* fp_constants,size_t fp_bytes,
    const float lod_by_texture[16],rsx_scene_alpha_sample_fn sample,void* user,
    rsx_scene_alpha_result* result);

#ifdef __cplusplus
}
#endif
#endif

#ifndef RSX_FSR1_CONSTANTS_H
#define RSX_FSR1_CONSTANTS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Push-constant words for AMD's FP32 EASU (64 bytes) and RCAS (16 bytes).
 * Input and output are complete, top-left aligned display-encoded images.
 * RCAS sharpness is in stops: 0 is strongest, 1 halves sharpening. */
typedef struct rsx_fsr1_constants {
    uint32_t easu[4][4];
    uint32_t rcas[4];
} rsx_fsr1_constants;

/* Returns 0 on success. Invalid input leaves *out unchanged. Device image
 * limits, color format and supported mode selection belong to the backend. */
int rsx_fsr1_make_constants(rsx_fsr1_constants *out,
    uint32_t input_width, uint32_t input_height,
    uint32_t output_width, uint32_t output_height, float sharpness_stops);

#ifdef __cplusplus
}
#endif
#endif

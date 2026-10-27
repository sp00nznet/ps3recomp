/* See at3_bridge.h. */
#include "at3_bridge.h"
#include "at3_decoders.h"

extern "C" void* at3p_open(int channels, int* block_align) { return atrac3p_alloc(channels, block_align); }
extern "C" void  at3p_close(void* dec) { atrac3p_free((ATRAC3PContext*)dec); }
extern "C" void  at3p_flush(void* dec) { atrac3p_flush_buffers((ATRAC3PContext*)dec); }
extern "C" int   at3p_decode(void* dec, float* left, float* right, int* samples,
                             const uint8_t* frame, int frame_bytes)
{
    float* out[2] = { left, right };
    return atrac3p_decode_frame((ATRAC3PContext*)dec, out, samples, frame, frame_bytes);
}

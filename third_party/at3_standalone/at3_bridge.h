/* C entry points to the (C++) ATRAC3plus decoder, for libs/codec/cellAtrac.c.
 * This bridge is ps3recomp's own (MIT); the decoder around it is LGPL-2.1. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void* at3p_open(int channels, int* block_align);
void  at3p_close(void* dec);
void  at3p_flush(void* dec);
/* Decodes one frame into planar float (right unused for mono). Returns bytes
 * consumed or < 0 on error; *samples gets the per-channel count. */
int   at3p_decode(void* dec, float* left, float* right, int* samples,
                  const uint8_t* frame, int frame_bytes);
#ifdef __cplusplus
}
#endif

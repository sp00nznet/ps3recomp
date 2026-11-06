/* Stand-in for the ATRAC3plus decoder (third_party/at3_standalone, LGPL-2.1) in builds that must stay free of LGPL
 * code, e.g. a single statically linked binary under a permissive license (PS3RECOMP_ATRAC_LGPL=OFF).
 * at3p_open reports failure, so cellAtrac refuses to set up a track and the title sees an ATRAC error. */
#include "../at3_standalone/at3_bridge.h"
#include <stddef.h>

void* at3p_open(int channels, int* block_align) { (void)channels; (void)block_align; return NULL; }
void  at3p_close(void* dec) { (void)dec; }
void  at3p_flush(void* dec) { (void)dec; }
int   at3p_decode(void* dec, float* left, float* right, int* samples, const uint8_t* frame, int frame_bytes)
{
    (void)dec; (void)left; (void)right; (void)samples; (void)frame; (void)frame_bytes;
    return -1;
}

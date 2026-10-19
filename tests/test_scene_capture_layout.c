/* CPU-only regression for the production FTX1 payload span calculation.
 * The layout helper has no renderer or guest-memory dependencies. */
#include "../libs/video/rsx_scene_texture_layout.h"

#include <stdio.h>

static unsigned checks,failures;
#define EXPECT_SPAN(fmt,w,h,mips,pitch,cube,want) do { \
    const u32 got=rsx_scene_texture_capture_span(fmt,w,h,mips,pitch,cube); \
    ++checks; \
    if(got!=(u32)(want)) { \
        fprintf(stderr,"line %d: span %u, expected %u\n",__LINE__,got,(u32)(want)); \
        ++failures; \
    } \
} while(0)

int main(void)
{
    /* Linear RGBA8: the 16-byte guest pitch applies to both level-0 rows. */
    EXPECT_SPAN(0xA5u,3,2,1,16,0,32);

    /* BC1: 8x8 is four 8-byte blocks; 4x4 and 2x2 each occupy one. */
    EXPECT_SPAN(0x86u,8,8,3,0,0,32+8+8);

    /* RGBA8 cube: each face is 4x4 + 2x2 + 1x1 = 84 bytes,
     * independently rounded to 128 before the next face starts. */
    EXPECT_SPAN(0x85u,4,4,3,0,1,6*128);

    EXPECT_SPAN(0x85u,0,4,3,0,1,0);
    EXPECT_SPAN(0x85u,4,0,3,0,1,0);

    /* A 2048-square RGBA8 payload fits exactly; the next size exceeds it. */
    EXPECT_SPAN(0x85u,2048,2048,1,0,0,16u*1024u*1024u);
    EXPECT_SPAN(0x85u,4096,4096,1,0,0,0);

    /* The layout layer stores face_bytes in u32. A huge linear pitch can
     * wrap that field; the capture helper must reject its u64 recomputation. */
    EXPECT_SPAN(0xA5u,1,2,1,0xffffffffu,0,0);

    printf("scene capture layout: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}

/* A swizzled texture decodes to exactly what its linearized copy decodes to,
 * for every uncompressed format and many sizes, including non-power-of-two
 * ones and sizes past the decoder's 4096-entry offset table. The linear path
 * is the reference: it reads texel (x, y) at y * row_bytes + x * bpp, while
 * the swizzled path combines per-row and per-column Morton offsets. */
#include "../libs/video/rsx_texture_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)

int main(void)
{
    static const u32 fmts[] = { 0x81, 0x82, 0x83, 0x84, 0x85, 0x8B, 0x8C, 0x8D, 0x90, 0x92, 0x94,
                                0x95, 0x97, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F };
    static const u32 dims[][2] = { {1,1}, {2,1}, {1,2}, {3,5}, {4,4}, {8,2}, {2,8}, {16,16}, {17,9},
                                   {64,32}, {32,64}, {100,60}, {128,128}, {256,4}, {4,256},
                                   {1000,3}, {4096,2}, {4100,2}, {2,4100} };
    for (u32 fi = 0; fi < sizeof fmts / sizeof fmts[0]; fi++)
    for (u32 di = 0; di < sizeof dims / sizeof dims[0]; di++)
    for (int rgba = 0; rgba < 2; rgba++) {
        const u32 w = dims[di][0], h = dims[di][1];
        rsx_tex_layout sw, ln;
        rsx_texture_layout(fmts[fi], w, h, &sw);
        if (!sw.swizzled || sw.compressed) continue;
        const u32 bpp = sw.bytes_per_texel;
        rsx_texture_layout_pitched(fmts[fi] | 0x20u, w, h, w * bpp, &ln);
        CHECK(!ln.swizzled && ln.bytes_per_texel == bpp && ln.dst_bytes_per_texel == sw.dst_bytes_per_texel);
        const u32 l2w = rsx_log2_ceil(w), l2h = rsx_log2_ceil(h);
        const size_t span = ((size_t)1 << l2w) * ((size_t)1 << l2h) * bpp;
        u8* src = malloc(span); u8* lin = malloc((size_t)w * h * bpp);
        const u32 dp = w * sw.dst_bytes_per_texel + 3u;
        u8* a = malloc((size_t)dp * h); u8* b = malloc((size_t)dp * h);
        CHECK(src && lin && a && b);
        for (size_t i = 0; i < span; i++) src[i] = (u8)(i * 131u + (i >> 9) * 7u + fi);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
                memcpy(lin + ((size_t)y * w + x) * bpp,
                       src + (size_t)rsx_swizzle_offset(x, y, l2w, l2h) * bpp, bpp);
        memset(a, 0xA5, (size_t)dp * h); memset(b, 0xA5, (size_t)dp * h);
        rsx_texture_decode(a, dp, lin, w, h, &ln, rgba);
        rsx_texture_decode(b, dp, src, w, h, &sw, rgba);
        CHECK(!memcmp(a, b, (size_t)dp * h));
        free(src); free(lin); free(a); free(b);
    }
    printf("PASS %u swizzled-vs-linear decode checks\n", checks);
    return 0;
}

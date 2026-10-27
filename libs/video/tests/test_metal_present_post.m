/* The draw engine's present pass with PS3_AA=fxaa and PS3_SHARPEN: what each
 * one does to a small synthetic display buffer, and that with both off the
 * present is still an exact copy. Needs a Metal device; no window.
 *
 *   clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_present_post.m \
 *     -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore \
 *     -o /tmp/test_metal_present_post && /tmp/test_metal_present_post
 */
#include "../rsx_metal_backend.m"
#include <assert.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }

enum { W = 32, H = 32 };
typedef unsigned char px4[4];          /* BGRA8 */

static id<MTLTexture> tex(MTLTextureUsage usage)
{
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:W height:H mipmapped:NO];
    d.usage = usage;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:d];
    assert(t);
    return t;
}

/* One present of `in` with the given options; the result in `out`. */
static void present(const px4* in, px4* out, int fxaa, float sharpen)
{
    id<MTLTexture> src = tex(MTLTextureUsageShaderRead);
    id<MTLTexture> dst = tex(MTLTextureUsageRenderTarget);
    [src replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:in bytesPerRow:W * 4];
    s_eng_fxaa = fxaa;
    s_eng_sharpen = sharpen;
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    eng_blit_to_display(cb, src, dst);
    [cb commit];
    [cb waitUntilCompleted];
    assert(cb.status == MTLCommandBufferStatusCompleted);
    [dst getBytes:out bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
}

static void grey(px4 p, unsigned v, unsigned a) { p[0] = p[1] = p[2] = (unsigned char)v; p[3] = (unsigned char)a; }

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        assert(s_dev);
        s_queue = [s_dev newCommandQueue];
        assert(s_queue);
        assert(eng_init(NULL, W, H) == 0);

        static px4 in[W * H], out[W * H];

        /* A shallow staircase (white where x > 2y + 3), alpha 0x80, and a
         * second, softer test image below. */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                grey(in[y * W + x], x > 2 * (y % 16) + 3 ? 255 : 0, 0x80);

        /* Both off: an exact copy. */
        present(in, out, 0, 0.0f);
        assert(!memcmp(in, out, sizeof in));

        /* FXAA: pixels on the staircase take in-between values, pixels two or
         * more away from any edge do not change, and alpha is kept. */
        present(in, out, 1, 0.0f);
        int blended = 0;
        for (int y = 1; y < 15; y++)
            for (int x = 1; x < W - 1; x++) {
                const unsigned char* o = out[y * W + x];
                int near = 0;
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= W || yy >= 16) continue;
                        near |= in[yy * W + xx][0] != in[y * W + x][0];
                    }
                if (!near) assert(!memcmp(o, in[y * W + x], 4));
                if (o[0] > 8 && o[0] < 247) blended++;
                assert(o[3] == 0x80);
            }
        printf("FXAA: %d staircase pixels blended\n", blended);
        assert(blended >= 10);

        /* CAS: a soft edge (64, 80, 128, 176, 192 across the columns) gets
         * steeper: its dark shoulder darker, its bright shoulder brighter,
         * the middle and the flat areas unchanged. (CAS holds back where a
         * neighbour is already at 0 or 255, so the edge sits mid-range.) */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                grey(in[y * W + x], x < 14 ? 64 : x == 14 ? 80 : x == 15 ? 128 : x == 16 ? 176 : 192, 0xFF);
        present(in, out, 0, 1.0f);
        const int row = 8 * W;
        printf("CAS 1.0: 80 -> %u, 128 -> %u, 176 -> %u\n", out[row + 14][0], out[row + 15][0], out[row + 16][0]);
        assert(out[row + 14][0] < 80 && out[row + 16][0] > 176);
        assert(abs((int)out[row + 15][0] - 128) <= 2);
        assert(!memcmp(out[row + 5], in[row + 5], 4) && !memcmp(out[row + 26], in[row + 26], 4));
        /* A weaker setting sharpens less. */
        const unsigned strong14 = out[row + 14][0], strong16 = out[row + 16][0];
        present(in, out, 0, 0.2f);
        printf("CAS 0.2: 80 -> %u, 176 -> %u\n", out[row + 14][0], out[row + 16][0]);
        assert(out[row + 14][0] < 80 && out[row + 14][0] > strong14);
        assert(out[row + 16][0] > 176 && out[row + 16][0] < strong16);

        /* Both together: FXAA first, then the sharpening, and still nothing
         * changes where the image is flat. */
        present(in, out, 1, 1.0f);
        assert(!memcmp(out[row + 5], in[row + 5], 4) && !memcmp(out[row + 26], in[row + 26], 4));

        puts("test_metal_present_post: ok");
    }
    return 0;
}

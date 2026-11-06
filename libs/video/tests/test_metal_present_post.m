/* The draw engine's present pass with PS3_AA=fxaa / smaa, PS3_SHARPEN,
 * PS3_VIBRANCE and PS3_CONTRAST: what each one does to a small synthetic
 * display buffer, and that with all of them off the present is still an
 * exact copy. Needs a Metal device; no window.
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

/* One present of `in` with the given options (aa: 0 off, 1 FXAA, 2 SMAA);
 * the result in `out`. */
enum { AA_OFF, AA_FXAA, AA_SMAA };
static void present_ex(const px4* in, px4* out, int aa, float sharpen, float vibrance, float contrast)
{
    id<MTLTexture> src = tex(MTLTextureUsageShaderRead);
    id<MTLTexture> dst = tex(MTLTextureUsageRenderTarget);
    [src replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:in bytesPerRow:W * 4];
    s_eng_fxaa = aa == AA_FXAA;
    s_eng_smaa = aa == AA_SMAA;
    s_eng_sharpen = sharpen;
    s_eng_vibrance = vibrance;
    s_eng_contrast = contrast;
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    eng_blit_to_display(cb, src, dst);
    [cb commit];
    [cb waitUntilCompleted];
    assert(cb.status == MTLCommandBufferStatusCompleted);
    [dst getBytes:out bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
}
static void present(const px4* in, px4* out, int aa, float sharpen)
{
    present_ex(in, out, aa, sharpen, 0.0f, 1.0f);
}

/* Pixels of the staircase rows (1..14) that took an in-between value; every
 * pixel two or more away from any edge must be unchanged, and alpha kept. */
static int staircase_blended(const px4* in, const px4* out)
{
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
    return blended;
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
        present(in, out, AA_OFF, 0.0f);
        assert(!memcmp(in, out, sizeof in));

        /* FXAA: pixels on the staircase take in-between values, pixels two or
         * more away from any edge do not change, and alpha is kept. */
        present(in, out, AA_FXAA, 0.0f);
        int blended = staircase_blended(in, out);
        printf("FXAA: %d staircase pixels blended\n", blended);
        assert(blended >= 10);

        /* SMAA: the same -- the staircase softened, the flat areas and the
         * alpha left alone. */
        present(in, out, AA_SMAA, 0.0f);
        assert(s_eng_smaa);                  /* its shaders compiled and ran */
        blended = staircase_blended(in, out);
        printf("SMAA: %d staircase pixels blended\n", blended);
        assert(blended >= 10);
        /* ...and a flat image goes through it unchanged. */
        { static px4 flat[W * H], fo[W * H];
          for (int i = 0; i < W * H; i++) grey(flat[i], 90, 0xFF);
          present(flat, fo, AA_SMAA, 0.0f);
          assert(!memcmp(flat, fo, sizeof flat)); }

        /* CAS: a soft edge (64, 80, 128, 176, 192 across the columns) gets
         * steeper: its dark shoulder darker, its bright shoulder brighter,
         * the middle and the flat areas unchanged. (CAS holds back where a
         * neighbour is already at 0 or 255, so the edge sits mid-range.) */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                grey(in[y * W + x], x < 14 ? 64 : x == 14 ? 80 : x == 15 ? 128 : x == 16 ? 176 : 192, 0xFF);
        present(in, out, AA_OFF, 1.0f);
        const int row = 8 * W;
        printf("CAS 1.0: 80 -> %u, 128 -> %u, 176 -> %u\n", out[row + 14][0], out[row + 15][0], out[row + 16][0]);
        assert(out[row + 14][0] < 80 && out[row + 16][0] > 176);
        assert(abs((int)out[row + 15][0] - 128) <= 2);
        assert(!memcmp(out[row + 5], in[row + 5], 4) && !memcmp(out[row + 26], in[row + 26], 4));
        /* A weaker setting sharpens less. */
        const unsigned strong14 = out[row + 14][0], strong16 = out[row + 16][0];
        present(in, out, AA_OFF, 0.2f);
        printf("CAS 0.2: 80 -> %u, 176 -> %u\n", out[row + 14][0], out[row + 16][0]);
        assert(out[row + 14][0] < 80 && out[row + 14][0] > strong14);
        assert(out[row + 16][0] > 176 && out[row + 16][0] < strong16);

        /* Both together: FXAA first, then the sharpening, and still nothing
         * changes where the image is flat. */
        present(in, out, AA_FXAA, 1.0f);
        assert(!memcmp(out[row + 5], in[row + 5], 4) && !memcmp(out[row + 26], in[row + 26], 4));

        /* Vibrance and contrast. Three pixels per row: a dull red (BGRA
         * 100,100,140), a pure red and mid-grey. */
        for (int i = 0; i < W * H; i++) {
            unsigned char* q = in[i];
            const int k = (i % W) % 3;
            if (k == 0)      { q[0] = 100; q[1] = 100; q[2] = 140; }
            else if (k == 1) { q[0] = 0;   q[1] = 0;   q[2] = 255; }
            else             { q[0] = q[1] = q[2] = 128; }
            q[3] = 0xFF;
        }
        /* At 0 and 1 they are off: an exact copy. */
        present_ex(in, out, AA_OFF, 0.0f, 0.0f, 1.0f);
        assert(!memcmp(in, out, sizeof in));
        /* Vibrance 0.5: the dull red gets redder (R up, G and B down, by
         * ~1.42x its distance from luma: 140 -> ~152, 100 -> ~95), the pure
         * red cannot get more saturated and stays, grey stays grey. */
        present_ex(in, out, AA_OFF, 0.0f, 0.5f, 1.0f);
        printf("vibrance 0.5: (140,100,100) -> (%u,%u,%u)\n", out[0][2], out[0][1], out[0][0]);
        assert(out[0][2] >= 149 && out[0][2] <= 155 && out[0][1] >= 92 && out[0][1] <= 97);
        assert(out[0][0] == out[0][1]);
        assert(!memcmp(out[1], in[1], 4) && !memcmp(out[2], in[2], 4));
        /* Vibrance -1: the dull red goes most of the way to its grey. */
        present_ex(in, out, AA_OFF, 0.0f, -1.0f, 1.0f);
        printf("vibrance -1: (140,100,100) -> (%u,%u,%u)\n", out[0][2], out[0][1], out[0][0]);
        assert(out[0][2] - out[0][1] <= 8 && out[0][2] > out[0][1]);
        assert(!memcmp(out[2], in[2], 4));
        /* Contrast around mid-grey: 1.5 pushes 100 down and 140 up by half
         * their distance from 127.5, 0.5 pulls them halfway in; grey stays. */
        present_ex(in, out, AA_OFF, 0.0f, 0.0f, 1.5f);
        printf("contrast 1.5: 140 -> %u, 100 -> %u, 128 -> %u\n", out[0][2], out[0][0], out[2][0]);
        assert(abs((int)out[0][2] - 146) <= 1 && abs((int)out[0][0] - 86) <= 1);
        assert(abs((int)out[2][0] - 128) <= 1);
        present_ex(in, out, AA_OFF, 0.0f, 0.0f, 0.5f);
        printf("contrast 0.5: 140 -> %u, 100 -> %u\n", out[0][2], out[0][0]);
        assert(abs((int)out[0][2] - 134) <= 1 && abs((int)out[0][0] - 114) <= 1);
        assert(abs((int)out[2][0] - 128) <= 1);

        puts("test_metal_present_post: ok");
    }
    return 0;
}

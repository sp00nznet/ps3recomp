/* The draw engine's present with PS3_UPSCALE=metalfx: a small display buffer
 * shown in a window twice its size goes through a MetalFX spatial scaler to
 * the shown size, and the present samples that 1:1; a letterboxed window
 * gets the fitted size with black around it; a window that is not larger,
 * and the headless target, are left alone. With PS3_UPSCALE unset a larger
 * window gets the bilinear stretch it always got. Where MetalFX is not
 * available (an older system or GPU) the option turns itself off and the
 * same frame falls back to that stretch, which is checked instead.
 * Needs a Metal device; no window -- the drawable is an offscreen texture.
 *
 *   clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_upscale.m \
 *     -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore \
 *     -o /tmp/test_metal_upscale && /tmp/test_metal_upscale
 */
#include "../rsx_metal_backend.m"
#include <assert.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }

enum { W = 32, H = 32 };
typedef unsigned char px4[4];          /* BGRA8 */

static id<MTLTexture> tex(NSUInteger w, NSUInteger h, MTLTextureUsage usage)
{
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    d.usage = usage;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:d];
    assert(t);
    return t;
}

/* One windowed present of the W x H image `in` into a dw x dh drawable. The
 * display buffer has the usage the engine's surfaces have. */
static void present(const px4* in, px4* out, NSUInteger dw, NSUInteger dh)
{
    id<MTLTexture> src = tex(W, H, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
    id<MTLTexture> dst = tex(dw, dh, MTLTextureUsageRenderTarget);
    [src replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:in bytesPerRow:W * 4];
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    eng_blit_to_display(cb, src, dst);
    [cb commit];
    [cb waitUntilCompleted];
    assert(cb.status == MTLCommandBufferStatusCompleted);
    [dst getBytes:out bytesPerRow:dw * 4 fromRegion:MTLRegionMake2D(0, 0, dw, dh) mipmapLevel:0];
}

/* PS3_UPSCALE as the next present reads it. */
static void upscale_env(const char* v)
{
    if (v) setenv("PS3_UPSCALE", v, 1); else unsetenv("PS3_UPSCALE");
    s_eng_upscale = -1;
}

static int near(int a, int b, int tol) { return abs(a - b) <= tol; }

/* The plain bilinear 2x stretch of the B channel at (x, y): pixel centres of
 * the output fall a quarter texel off the input's, edges clamp. */
static int bilinear2x(const px4* in, int x, int y)
{
    const double fx = (x + 0.5) / 2.0 - 0.5, fy = (y + 0.5) / 2.0 - 0.5;
    const int x0 = (int)floor(fx), y0 = (int)floor(fy);
    const double ax = fx - x0, ay = fy - y0;
    int xs[2] = { x0, x0 + 1 }, ys[2] = { y0, y0 + 1 };
    for (int k = 0; k < 2; k++) {
        xs[k] = xs[k] < 0 ? 0 : xs[k] >= W ? W - 1 : xs[k];
        ys[k] = ys[k] < 0 ? 0 : ys[k] >= H ? H - 1 : ys[k];
    }
    const double v = (1 - ay) * ((1 - ax) * in[ys[0] * W + xs[0]][0] + ax * in[ys[0] * W + xs[1]][0]) +
                     ay * ((1 - ax) * in[ys[1] * W + xs[0]][0] + ax * in[ys[1] * W + xs[1]][0]);
    return (int)floor(v + 0.5);
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        assert(s_dev);
        s_queue = [s_dev newCommandQueue];
        assert(s_queue);
        assert(eng_init(NULL, W, H) == 0);
        assert(create_placeholders() == 0);    /* the window's bilinear sampler */

        /* Dark left, bright right, the edge between columns 15 and 16, and a
         * mid-grey square in the bright half; opaque. */
        static px4 in[W * H];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                unsigned v = x < 16 ? 40 : (x >= 22 && x < 28 && y >= 10 && y < 16) ? 128 : 210;
                px4* p = &in[y * W + x];
                (*p)[0] = (*p)[1] = (*p)[2] = (unsigned char)v;
                (*p)[3] = 0xFF;
            }
        static px4 same[W * H], plain2x[4 * W * H], fx2x[4 * W * H], box[6 * W * H];

        /* PS3_UPSCALE unset: 1:1 is an exact copy, and twice the size is the
         * bilinear stretch, with no scaler made. */
        upscale_env(NULL);
        present(in, same, W, H);
        assert(s_eng_upscale == 0);
        assert(!memcmp(in, same, sizeof same));
        present(in, plain2x, 2 * W, 2 * H);
        assert(!s_eng_mfx_scaler && !s_eng_mfx_out);
        int worst = 0;
        for (int y = 0; y < 2 * H; y++)
            for (int x = 0; x < 2 * W; x++) {
                const int d = abs((int)plain2x[y * 2 * W + x][0] - bilinear2x(in, x, y));
                if (d > worst) worst = d;
            }
        printf("unset: 2x is the bilinear stretch (worst %d off)\n", worst);
        assert(worst <= 2);
        /* "off" and an unknown value are the same as unset. */
        static px4 tmp[4 * W * H];
        upscale_env("off");
        present(in, tmp, 2 * W, 2 * H);
        assert(s_eng_upscale == 0 && !memcmp(tmp, plain2x, sizeof tmp));
        upscale_env("dlss");
        present(in, tmp, 2 * W, 2 * H);
        assert(s_eng_upscale == 0 && !memcmp(tmp, plain2x, sizeof tmp));

        /* PS3_UPSCALE=metalfx, a window twice the size. */
        upscale_env("metalfx");
        present(in, fx2x, 2 * W, 2 * H);
        if (s_eng_upscale == 0) {
            /* MetalFX is not here: off, and this frame was the stretch. */
            puts("MetalFX not available on this system; checked the fallback");
            assert(!s_eng_mfx_scaler && !s_eng_mfx_out);
            assert(!memcmp(fx2x, plain2x, sizeof fx2x));
            puts("test_metal_upscale: ok (fallback)");
            return 0;
        }
        id<MTLTexture> out = s_eng_mfx_out;
        assert(s_eng_mfx_scaler && out);
        assert([out width] == 2 * W && [out height] == 2 * H);
        /* Not black, not the bilinear frame, and still the picture: the flat
         * areas well away from any edge keep their value, the dark half stays
         * dark and the bright half bright, alpha kept. */
        long sum = 0;
        int differs = 0;
        for (int i = 0; i < 4 * W * H; i++) {
            sum += fx2x[i][0];
            differs |= memcmp(fx2x[i], plain2x[i], 4) != 0;
            assert(fx2x[i][3] == 0xFF);
            assert(fx2x[i][0] == fx2x[i][1] && fx2x[i][1] == fx2x[i][2]);
        }
        const double mean = (double)sum / (4 * W * H);
        printf("metalfx: 2x mean %.1f (bilinear differs: %s), dark %u, bright %u, square %u\n", mean,
               differs ? "yes" : "no", fx2x[40 * 2 * W + 10][0], fx2x[50 * 2 * W + 60][0],
               fx2x[26 * 2 * W + 50][0]);
        assert(mean > 100.0 && mean < 150.0);
        assert(differs);
        assert(near(fx2x[40 * 2 * W + 10][0], 40, 3));     /* the dark half */
        assert(near(fx2x[50 * 2 * W + 60][0], 210, 3));    /* the bright half */
        assert(near(fx2x[26 * 2 * W + 50][0], 128, 3));    /* inside the square */
        for (int y = 0; y < 2 * H; y++) {
            assert(fx2x[y * 2 * W + 26][0] < 90);           /* left of the edge */
            assert(fx2x[y * 2 * W + 37][0] > 160);          /* right of it */
        }

        /* The same window again reuses the scaler; a letterboxed one (three
         * times as wide as it is tall twice) gets a 2x picture in the middle
         * and black at the sides. */
        id scaler = s_eng_mfx_scaler;
        present(in, tmp, 2 * W, 2 * H);
        assert(s_eng_mfx_scaler == scaler && !memcmp(tmp, fx2x, sizeof tmp));
        present(in, box, 3 * W, 2 * H);
        assert([s_eng_mfx_out width] == 2 * W && [s_eng_mfx_out height] == 2 * H);
        for (int y = 0; y < 2 * H; y++)
            for (int x = 0; x < 3 * W; x++) {
                const unsigned char* p = box[y * 3 * W + x];
                if (x < W / 2 || x >= W / 2 + 2 * W) assert(p[0] == 0 && p[1] == 0 && p[2] == 0);
                else assert(!memcmp(p, fx2x[y * 2 * W + x - W / 2], 4));
            }
        puts("metalfx: letterboxed window gets the fitted size, black around it");

        /* A window no larger than the picture: an exact copy, MetalFX not run
         * (nor rebuilt for it). */
        scaler = s_eng_mfx_scaler;
        present(in, same, W, H);
        assert(!memcmp(in, same, sizeof same) && s_eng_mfx_scaler == scaler);

        /* Headless: the target is filled as before and never upscaled. */
        s_eng_mfx_scaler = nil; s_eng_mfx_out = nil;
        s_headless = 1;
        present(in, tmp, 2 * W, 2 * H);
        s_headless = 0;
        assert(!s_eng_mfx_scaler && !s_eng_mfx_out && s_eng_upscale == 1);
        puts("metalfx: not used headless or when the window is not larger");

        eng_shutdown(NULL);
        assert(!s_eng_mfx_scaler && !s_eng_mfx_out);
        puts("test_metal_upscale: ok");
    }
    return 0;
}

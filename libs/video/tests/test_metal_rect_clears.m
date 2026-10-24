/*
 * The Metal backend's rectangle clears (rsx_draw_backend.clear_color_rect,
 * clear_depth_stencil_rect): a clear limited to part of a target leaves the
 * rest of it as it was. Resistance: Fall of Man's co-op split screen clears
 * player 2's half of the scene target after player 1's view is drawn into
 * the other half; a clear folded into a pass's load action covers the whole
 * attachment and took player 1's view with it.
 *
 * Run by tools/test_metal_rect_clears.py, or:
 *   clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_rect_clears.m \
 *     libs/video/rsx_texture_layout.c -Wl,-dead_strip \
 *     -framework Metal -framework Cocoa -framework QuartzCore -o /tmp/test_metal_rect_clears
 */
#include "../rsx_metal_backend.m"

static int g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) { printf("[PASS] " __VA_ARGS__); printf("\n"); }            \
        else { printf("[FAIL] " __VA_ARGS__); printf("\n"); g_fail++; }       \
    } while (0)

#define W 8u
#define H 4u

static id<MTLTexture> color_texture(void)
{
    MTLTextureDescriptor* d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:W height:H mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    return [s_dev newTextureWithDescriptor:d];
}

static id<MTLTexture> depth_texture(void)
{
    MTLTextureDescriptor* d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTL_DEPTH_FORMAT
                                                           width:W height:H mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    return [s_dev newTextureWithDescriptor:d];
}

static int encode_and_wait(void)
{
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    eng_encode_records(cb, nil);
    s_eng_rec_count = 0;
    [cb commit];
    [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted;
}

/* Columns [0, split) hold `left`, [split, W) hold `right` (R,G,B,A). */
static int color_halves(id<MTLTexture> t, u32 split, const u8 left[4], const u8 right[4])
{
    u8 px[W * H * 4];
    [t getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++)
            if (memcmp(px + (y * W + x) * 4, x < split ? left : right, 4)) return 0;
    return 1;
}

/* Depth and stencil per column as above. */
static int depth_halves(id<MTLTexture> t, u32 split, float zl, u8 sl, float zr, u8 sr)
{
    const NSUInteger drow = W * 4, srow = W, dsize = drow * H, ssize = srow * H;
    id<MTLBuffer> db = [s_dev newBufferWithLength:dsize options:MTLResourceStorageModeShared];
    id<MTLBuffer> sb = [s_dev newBufferWithLength:ssize options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(W, H, 1) toBuffer:db destinationOffset:0
   destinationBytesPerRow:drow destinationBytesPerImage:dsize
                  options:MTLBlitOptionDepthFromDepthStencil];
    [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(W, H, 1) toBuffer:sb destinationOffset:0
   destinationBytesPerRow:srow destinationBytesPerImage:ssize
                  options:MTLBlitOptionStencilFromDepthStencil];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) {
            float z;
            memcpy(&z, (const u8*)db.contents + y * drow + x * 4, 4);
            const u8 s = *((const u8*)sb.contents + y * srow + x);
            if (fabsf(z - (x < split ? zl : zr)) > 1e-6f || s != (x < split ? sl : sr))
                return 0;
        }
    return 1;
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("no Metal device: skipped"); return 0; }
        s_queue = [s_dev newCommandQueue];
        if (eng_init(NULL, W, H) != 0) { puts("[FAIL] eng_init"); return 1; }

        id<MTLTexture> c = color_texture();
        id<MTLTexture> z = depth_texture();
        const u32 hc = eng_obj_add(c), hz = eng_obj_add(z);
        const float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 };
        const u8 r8[4] = { 255, 0, 0, 255 }, g8[4] = { 0, 255, 0, 255 };

        /* Player 1's half, then player 2's clear of the other half. */
        eng_clear_color(NULL, hc, red);
        eng_clear_depth_stencil(NULL, hz, RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 0.25f, 7);
        eng_clear_color_rect(NULL, hc, green, W / 2, 0, W / 2, H);
        eng_clear_depth_stencil_rect(NULL, hz, RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL,
                                     0.75f, 11, W / 2, 0, W / 2, H);
        CHECK(encode_and_wait(), "the frame with rectangle clears executes");
        CHECK(color_halves(c, W / 2, r8, g8),
              "a colour clear of the right half keeps the left half");
        CHECK(depth_halves(z, W / 2, 0.25f, 7, 0.75f, 11),
              "a depth and stencil clear of the right half keeps the left half");

        /* Depth alone leaves the stencil; a rectangle past the target's edge
         * is clipped to it. */
        eng_clear_depth_stencil_rect(NULL, hz, RSX_BE_CLEAR_DEPTH, 0.5f, 99, 0, 0, W / 2, H * 4);
        CHECK(encode_and_wait(), "a depth-only rectangle clear executes");
        CHECK(depth_halves(z, W / 2, 0.5f, 7, 0.75f, 11),
              "a depth-only clear keeps the stencil and the other half");

        /* A whole clear queued right after a rectangle one still covers the
         * whole target, and the rectangle is not folded into it. */
        eng_clear_color_rect(NULL, hc, red, 0, 0, 1, 1);
        eng_clear_color(NULL, hc, green);
        CHECK(encode_and_wait() && color_halves(c, W, g8, g8),
              "a whole clear after a rectangle clear covers the target");
    }
    printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}

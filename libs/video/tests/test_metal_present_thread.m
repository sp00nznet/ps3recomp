/* The opt-in present thread (PS3RECOMP_RSX_PRESENT_THREAD=1) on real Metal:
 * a present stages the flipped surface and returns; the present thread shows
 * it. Headless by default (the show goes into the offscreen target); "layer"
 * shows into a CAMetalLayer that is in no window, which hands out drawables
 * all the same. Synthetic 2x1 surfaces only. Built and run by
 * tools/test_metal_present_thread.py. */
#undef NDEBUG
#include "../rsx_metal_backend.m"
#include <assert.h>
#include <unistd.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }

static id<MTLTexture> texture(const unsigned char* pixels)
{
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:2 height:1 mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:d];
    assert(t);
    [t replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0 withBytes:pixels bytesPerRow:8];
    return t;
}

static void read_offscreen(unsigned char out[8])
{
    [s_offscreen getBytes:out bytesPerRow:8 fromRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0];
}

/* The show behind a gate the test opens, so a show can be held back past
 * later GPU work on the presenting thread. */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_gate_closed, g_entered;
static pthread_t g_presenting;
static int gated_show(void* user, uint32_t slot, rsx_present_show_timing* t)
{
    assert(!pthread_equal(pthread_self(), g_presenting));
    pthread_mutex_lock(&g_mu);
    g_entered++;
    pthread_cond_broadcast(&g_cv);
    while (g_gate_closed) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    return eng_pt_show(user, slot, t);
}
static void gate(int closed)
{
    pthread_mutex_lock(&g_mu);
    g_gate_closed = closed;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}
static void wait_entered(int n)
{
    pthread_mutex_lock(&g_mu);
    while (g_entered < n) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
}
static void wait_presented(uint64_t n)
{
    for (int i = 0; i < 5000; i++) {
        rsx_present_thread_stats st;
        rsx_present_thread_get_stats(s_pt, &st);
        if (st.presented >= n) return;
        usleep(1000);
    }
    assert(!"the present thread did not show the staged frame");
}

static void* open_later(void* p)
{
    (void)p;
    usleep(50000);              /* shutdown is waiting for the held show */
    gate(0);
    return NULL;
}

/* The windowed show (in-flight semaphore, drawable size, nextDrawable,
 * presentDrawable) on a detached layer. A window resize in between recreates
 * the staging textures and the drawable follows them. */
static int run_layer(void)
{
    s_headless = 0;
    s_width = 2; s_height = 1;
    s_win_px_w = 2; s_win_px_h = 1;
    s_layer = [CAMetalLayer layer];
    s_layer.device = s_dev;
    s_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    s_layer.framebufferOnly = YES;
    s_layer.drawableSize = CGSizeMake(2, 1);
    s_inflight = dispatch_semaphore_create(MTL_MAX_INFLIGHT);
    setenv("PS3RECOMP_RSX_PRESENT_THREAD_DEPTH", "2", 1);
    assert(eng_init(NULL, 2, 1) == 0);
    eng_pt_start();
    assert(s_pt);
    const unsigned char green[] = {0,0,0,255, 17,23,31,255};
    const u32 surface = eng_obj_add(texture(green));
    for (int i = 0; i < 30; i++) {
        if (i == 15) { s_win_px_w = 4; s_win_px_h = 2; }   /* staging follows the new size */
        eng_present(NULL, surface);
        usleep(2000);
    }
    rsx_present_thread_stats st;
    for (int i = 0; i < 5000; i++) {
        rsx_present_thread_get_stats(s_pt, &st);
        if (st.presented + st.show_failed + st.dropped >= 30) break;
        usleep(1000);
    }
    rsx_present_thread_get_stats(s_pt, &st);
    assert([s_pt_tex[0] width] == 4 || [s_pt_tex[1] width] == 4);
    assert(st.staged == 30 && st.depth == 2 && st.max_occupancy <= 2);
    assert(st.presented > 0 && st.presented + st.show_failed + st.dropped == 30);
    eng_shutdown(NULL);
    assert(s_layer.drawableSize.width == 4 && s_layer.drawableSize.height == 2);
    /* Every in-flight slot comes back: the shutdown drain must not hang. */
    for (int i = 0; i < MTL_MAX_INFLIGHT; i++) dispatch_semaphore_wait(s_inflight, DISPATCH_TIME_FOREVER);
    for (int i = 0; i < MTL_MAX_INFLIGHT; i++) dispatch_semaphore_signal(s_inflight);
    printf("Metal present thread PASS: layer presented=%llu show_failed=%llu dropped=%llu, resize\n",
           (unsigned long long)st.presented, (unsigned long long)st.show_failed, (unsigned long long)st.dropped);
    return 0;
}

int main(int argc, char** argv)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { printf("SKIP: no Metal device\n"); return 0; }
        s_queue = [s_dev newCommandQueue]; assert(s_queue);
        setenv("PS3RECOMP_RSX_PRESENT_THREAD_REPORT_S", "0", 1);
        if (argc > 1 && !strcmp(argv[1], "layer")) return run_layer();
        s_headless = 1;
        g_presenting = pthread_self();
        const unsigned char black[] = {0,0,0,255, 0,0,0,255};
        const unsigned char green[] = {0,0,0,255, 17,23,31,255};
        s_offscreen = texture(black);

        /* Off: nothing is created, and a present is the ordinary one. */
        assert(eng_init(NULL, 2, 1) == 0);
        assert(!s_pt);
        u32 surface = eng_obj_add(texture(green));
        eng_present(NULL, surface);
        unsigned char px[8];
        read_offscreen(px);
        assert(!memcmp(px, green, 8));
        eng_shutdown(NULL);
        s_offscreen = texture(black);

        assert(eng_init(NULL, 2, 1) == 0);
        eng_pt_start();
        assert(s_pt);

        /* 1. A present stages and returns; the present thread shows it. */
        surface = eng_obj_add(texture(green));
        eng_present(NULL, surface);
        wait_presented(1);
        read_offscreen(px);
        assert(!memcmp(px, green, 8));

        /* 2. Ordering: the staging copy runs before a later write to the same
         * surface, even when the show itself comes after that write. */
        rsx_present_thread_destroy(s_pt);
        const rsx_present_hooks hooks = { NULL, eng_pt_stage, gated_show, eng_pt_discard };
        s_pt = rsx_present_thread_create(&hooks, 3);
        assert(s_pt);
        u32 target = eng_obj_add(texture(black));
        gate(1);
        eng_present(NULL, target);                       /* stages black */
        wait_entered(1);                                 /* show held */
        const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
        eng_clear_color(NULL, target, red);              /* a later write */
        eng_submit_and_wait(NULL, 0);
        assert(s_eng_checked_completed);
        gate(0);
        wait_presented(1);
        read_offscreen(px);
        assert(!memcmp(px, black, 8));                   /* not the later red */
        eng_present(NULL, target);                       /* the next flip shows red */
        wait_presented(2);
        read_offscreen(px);
        const unsigned char red_px[] = {255,0,0,255, 255,0,0,255};
        assert(!memcmp(px, red_px, 8));

        /* 3. A held show never holds up the presenting thread: with every
         * slot taken the oldest staged frame is dropped, and the newest wins. */
        u32 frames[6];
        for (int i = 0; i < 6; i++) {
            const unsigned char c[] = {(unsigned char)(10*i),1,2,255, 3,4,5,255};
            frames[i] = eng_obj_add(texture(c));
        }
        gate(1);
        eng_present(NULL, frames[0]);
        wait_entered(3);
        for (int i = 1; i < 6; i++) eng_present(NULL, frames[i]);   /* returns at once */
        rsx_present_thread_stats st;
        rsx_present_thread_get_stats(s_pt, &st);
        assert(st.staged == 8 && st.dropped == 3 && st.max_occupancy == 3);
        gate(0);
        wait_presented(5);
        read_offscreen(px);
        const unsigned char last[] = {50,1,2,255, 3,4,5,255};
        assert(!memcmp(px, last, 8));
        rsx_present_thread_get_stats(s_pt, &st);
        assert(st.presented == 5 && st.show_failed == 0 && st.stage_failed == 0);

        /* 4. Shutdown with frames still staged: the thread joins and they are
         * discarded, not shown. */
        gate(1);
        eng_present(NULL, frames[1]);
        wait_entered(6);
        eng_present(NULL, frames[2]);
        eng_present(NULL, frames[3]);
        rsx_present_thread_get_stats(s_pt, &st);
        assert(st.staged == 11 && st.presented == 5);
        pthread_t opener;
        assert(!pthread_create(&opener, NULL, open_later, NULL));
        eng_shutdown(NULL);         /* joins once the held show finishes */
        assert(!pthread_join(opener, NULL));
        pthread_mutex_lock(&g_mu);
        assert(g_entered == 6);     /* frames 2 and 3 were never shown */
        pthread_mutex_unlock(&g_mu);
        assert(!s_pt && !s_pt_tex[0] && !s_pt_queue);
        printf("Metal present thread PASS: off is inert, stage+show, GPU order after a later write, "
               "drop oldest without blocking, shutdown\n");
    }
    return 0;
}

/* The window host off screen (host_window.m, "Off screen"): a window macOS
 * does not show (another app or Space in front, minimized, covered) logs
 * that it left and came back, its [RSX FPS] lines carry hidden=1 meanwhile,
 * and coming back shows the newest frame the game published while it was
 * away. No window: the layer is a bare CAMetalLayer.
 *
 *   clang -fobjc-arc -I libs/video tools/host_window/test_host_window_offscreen.m \
 *         -framework AppKit -framework Metal -framework QuartzCore -framework IOSurface \
 *         -o /tmp/test_host_window_offscreen && /tmp/test_host_window_offscreen
 */
#define main host_window_main
#include "host_window.m"
#undef main
#include <assert.h>

/* stderr into a file for the duration of fn; returns what was written */
static char* capture(void (^fn)(void))
{
    char path[] = "/tmp/test_host_window_offscreen.XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    fflush(stderr);
    const int saved = dup(2);
    dup2(fd, 2);
    fn();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    const off_t n = lseek(fd, 0, SEEK_END);
    char* out = calloc(1, (size_t)n + 1);
    pread(fd, out, (size_t)n, 0);
    close(fd);
    unlink(path);
    return out;
}

static int count(const char* s, const char* what)
{
    int n = 0;
    for (const char* p = s; (p = strstr(p, what)); p += strlen(what)) n++;
    return n;
}

/* what the game does for a finished frame */
static uint64_t publish(void)
{
    const int slot = rsx_host_begin_render(g.ctl);
    assert(slot >= 0);
    rsx_host_end_render(g.ctl, slot, 1, mach_absolute_time());
    return atomic_load(&g.ctl->seq[slot]);
}

/* the copy's completion comes back on the main queue */
static void settle(void)
{
    for (int i = 0; i < 500 && g.present_busy; i++)
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    assert(!g.present_busy);
}

int main(void)
{
    @autoreleasepool {
        g.dev = MTLCreateSystemDefaultDevice();
        if (!g.dev) { printf("test_host_window_offscreen: no Metal device, skipped\n"); return 0; }
        g.queue = [g.dev newCommandQueue];
        g.layer = [CAMetalLayer layer];
        g.layer.device = g.dev;
        g.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g.layer.framebufferOnly = NO;
        assert(make_control() == 0 && make_surfaces(64, 36) == 0);
        g.fps_on = 1;
        metal_frame_rate_reset(&g.fps, CACurrentMediaTime());

        /* on screen: a frame goes up, and the rate line is plain */
        const uint64_t first = publish();
        try_present();
        settle();
        assert(g.shown_seq == first);
        char* line = capture(^{ stats_tick(); });
        assert(strstr(line, "[RSX FPS] display=") && !strstr(line, "hidden"));
        free(line);

        /* off screen: said once, and every rate line says hidden=1 */
        line = capture(^{ note_on_screen(0, 100.0); note_on_screen(0, 101.0); });
        assert(count(line, "[host window] window off screen") == 1);
        free(line);
        assert(g.off_screen);
        line = capture(^{ stats_tick(); });
        assert(strstr(line, "[RSX FPS] display=") && strstr(line, " hidden=1\n"));
        free(line);

        /* the game goes on meanwhile; back on screen, the newest of its
         * frames goes up at once, with how long the window was away */
        settle();
        publish();
        const uint64_t newest = publish();
        line = capture(^{ note_on_screen(1, 127.5); });
        assert(strstr(line, "[host window] window on screen again after 27.5 s"));
        free(line);
        assert(!g.off_screen);
        assert(g.present_busy);                 /* presented by the change itself */
        settle();
        assert(g.shown_seq == newest);
        line = capture(^{ note_on_screen(1, 130.0); stats_tick(); });
        assert(!strstr(line, "on screen again") && !strstr(line, "hidden"));
        free(line);
        printf("test_host_window_offscreen: ok\n");
    }
    return 0;
}

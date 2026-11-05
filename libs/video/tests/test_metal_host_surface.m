/* clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_host_surface.m
 * libs/video/rsx_texture_layout.c libs/video/rsx_vertex_fetch.c libs/video/rsx_vp_decompiler.c
 * libs/video/rsx_fp_decompiler.c libs/video/rsx_shader_msl.cpp -lc++ -Wl,-dead_strip
 * -framework Metal -framework Cocoa -framework QuartzCore -framework IOSurface
 * -o /tmp/test_metal_host_surface
 *
 * Host-surface mode (PS3RECOMP_HOST_SURFACES, rsx_host_surface.h) against a
 * launcher fixture in this process: a bootstrap service on a thread that
 * hands out a control page and IOSurfaces the way tools/host_window does.
 *   - the surface state machine on its own (take, publish, reclaim, read);
 *   - init attaches and opens no window; an engine present lands in a
 *     surface, is published with a FRAME message, and reads back right;
 *   - presents nobody reads never stall: old frames are reclaimed;
 *   - the vtable path renders into the surface, and a display resize asks
 *     the launcher for surfaces of the new size;
 *   - `closed` makes the pump report the window gone;
 *   - a second init (the image an exec brings up) attaches again, frames the
 *     old image left RENDERING are freed, and the sequence keeps rising;
 *   - export_window_state hands nothing over. */
#include "../rsx_metal_backend.m"
#include <IOSurface/IOSurface.h>
#include <assert.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }
void rsx_set_backend(rsx_backend* b) { (void)b; }
void rsx_draw_engine_set_backend(const rsx_draw_backend* b) { (void)b; }
void rsx_draw_engine_set_default(int on) { (void)on; }
int  rsx_draw_engine_enabled(void) { return 1; }
int  rsx_draw_engine_init(u32 w, u32 h) { return eng_init(NULL, w, h); }
void rsx_draw_engine_shutdown(void) { eng_shutdown(NULL); }
void rsx_draw_engine_present(void) {}
u32  rsx_draw_engine_readback_center(void) { return 0; }
u32  rsx_draw_engine_guest_draws(void) { return 0; }
int  cellGcmOffsetIsDisplay(u32 off) { (void)off; return 1; }
/* The vtable path is linked in; no draw reaches guest memory here. */
u32  cellGcmResolveLocated(int location, u32 offset) { (void)location; return offset; }
u32  cellGcmResolveOffset(u32 offset) { return offset; }
void ps3_host_quit(const char* why) { (void)why; }   /* Cmd+Q in the backend (cellSysutil.c) */
u8*  vm_base;

/* ---- the launcher fixture ------------------------------------------------ */

static mach_port_t       f_service, f_entry;
static rsx_host_control* f_ctl;
static IOSurfaceRef      f_surf[RSX_HOST_SLOTS];
static u32               f_w, f_h;
static atomic_int        f_frames, f_attaches;

static void f_make_surfaces(u32 w, u32 h)
{
    for (int i = 0; i < RSX_HOST_SLOTS; i++) {
        if (f_surf[i]) CFRelease(f_surf[i]);
        NSDictionary* p = @{ (id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h),
                             (id)kIOSurfaceBytesPerElement: @4,
                             (id)kIOSurfacePixelFormat: @((u32)'BGRA') };
        f_surf[i] = IOSurfaceCreate((__bridge CFDictionaryRef)p);
        assert(f_surf[i]);
        if (atomic_load(&f_ctl->state[i]) != RSX_HOST_READING)
            atomic_store(&f_ctl->state[i], RSX_HOST_FREE);
    }
    f_w = w; f_h = h;
    f_ctl->width = w; f_ctl->height = h;
    f_ctl->generation++;
}

static void* f_thread(void* unused)
{
    (void)unused;
    for (;;) {
        struct { rsx_host_attach_msg m; mach_msg_trailer_t t; } in;
        memset(&in, 0, sizeof in);
        if (mach_msg(&in.m.header, MACH_RCV_MSG, 0, sizeof in, f_service, 0, MACH_PORT_NULL))
            continue;
        if (in.m.header.msgh_id == RSX_HOST_MSG_FRAME) { atomic_fetch_add(&f_frames, 1); continue; }
        assert(in.m.header.msgh_id == RSX_HOST_MSG_ATTACH);
        assert(in.m.pid == getpid());
        if (in.m.width && (in.m.width != f_w || in.m.height != f_h)) f_make_surfaces(in.m.width, in.m.height);
        else rsx_host_reset_rendering(f_ctl);
        rsx_host_surfaces_msg r;
        memset(&r, 0, sizeof r);
        r.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
        r.header.msgh_size = sizeof r;
        r.header.msgh_remote_port = in.m.header.msgh_remote_port;
        r.header.msgh_id = RSX_HOST_MSG_SURFACES;
        r.body.msgh_descriptor_count = 1 + RSX_HOST_SLOTS;
        r.control.name = f_entry; r.control.disposition = MACH_MSG_TYPE_COPY_SEND;
        r.control.type = MACH_MSG_PORT_DESCRIPTOR;
        for (int i = 0; i < RSX_HOST_SLOTS; i++) {
            r.surface[i].name = IOSurfaceCreateMachPort(f_surf[i]);
            r.surface[i].disposition = MACH_MSG_TYPE_MOVE_SEND;
            r.surface[i].type = MACH_MSG_PORT_DESCRIPTOR;
        }
        r.width = f_w; r.height = f_h; r.generation = f_ctl->generation; r.launcher_pid = getpid();
        assert(mach_msg(&r.header, MACH_SEND_MSG, sizeof r, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL) == KERN_SUCCESS);
        atomic_fetch_add(&f_attaches, 1);
    }
    return NULL;
}

static void f_start(void)
{
    char name[128];
    snprintf(name, sizeof name, "org.ps3recomp.test-host-surface.%d", getpid());
    assert(bootstrap_check_in(bootstrap_port, name, &f_service) == KERN_SUCCESS);
    vm_address_t a = 0;
    assert(vm_allocate(mach_task_self(), &a, RSX_HOST_CONTROL_BYTES, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    memory_object_size_t sz = RSX_HOST_CONTROL_BYTES;
    assert(mach_make_memory_entry_64(mach_task_self(), &sz, a, VM_PROT_READ | VM_PROT_WRITE,
                                     &f_entry, MACH_PORT_NULL) == KERN_SUCCESS);
    f_ctl = (rsx_host_control*)a;
    f_ctl->magic = RSX_HOST_MAGIC; f_ctl->version = RSX_HOST_VERSION; f_ctl->launcher_pid = getpid();
    f_make_surfaces(64, 32);
    pthread_t t;
    assert(pthread_create(&t, NULL, f_thread, NULL) == 0);
    setenv(RSX_HOST_SURFACE_ENV, name, 1);
}

/* Wait until `n` frames are published (a FRAME wake may be dropped when the
 * queue is full, so count publishes, not messages), then take the newest
 * frame as the launcher would and return its centre pixel (BGRA, as a
 * little-endian u32). */
static u32 f_read_newest(int n, uint64_t* seq_out)
{
    for (int i = 0; i < 400 && atomic_load(&f_ctl->published) < (uint64_t)n; i++) usleep(5000);
    assert(atomic_load(&f_ctl->published) >= (uint64_t)n);
    for (int i = 0; i < 400 && atomic_load(&f_frames) == 0; i++) usleep(5000);
    assert(atomic_load(&f_frames) > 0);
    static uint64_t shown;
    uint64_t seq = 0;
    const int slot = rsx_host_begin_read(f_ctl, shown, &seq);
    assert(slot >= 0 && seq > shown);
    shown = seq;
    IOSurfaceRef s = f_surf[slot];
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    const u8* base = IOSurfaceGetBaseAddress(s);
    u32 px;
    memcpy(&px, base + (f_h / 2) * IOSurfaceGetBytesPerRow(s) + (f_w / 2) * 4, 4);
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    rsx_host_end_read(f_ctl, slot);
    if (seq_out) *seq_out = seq;
    return px;
}

static id<MTLTexture> solid(u32 w, u32 h, u32 bgra)
{
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:d];
    u32* px = malloc((size_t)w * h * 4);
    for (u32 i = 0; i < w * h; i++) px[i] = bgra;
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:w * 4];
    free(px);
    return t;
}

/* ---- the state machine alone --------------------------------------------- */

static void check_states(void)
{
    static rsx_host_control c;
    memset(&c, 0, sizeof c);
    int r[RSX_HOST_SLOTS];
    for (int i = 0; i < RSX_HOST_SLOTS; i++) { r[i] = rsx_host_begin_render(&c); assert(r[i] == i); }
    assert(rsx_host_begin_render(&c) == -1 && c.skipped == 1);      /* all rendering */
    rsx_host_end_render(&c, r[0], 1, 1);
    rsx_host_end_render(&c, r[1], 1, 2);
    rsx_host_end_render(&c, r[2], 0, 3);                            /* failed frame: back to FREE */
    assert(c.state[2] == RSX_HOST_FREE && c.published == 2);
    assert(rsx_host_begin_render(&c) == 2);
    assert(rsx_host_begin_render(&c) == 0 && c.reclaimed == 1);     /* oldest READY taken back */
    uint64_t q = 0;
    assert(rsx_host_begin_read(&c, 0, &q) == 1 && q == c.seq[1]);
    assert(rsx_host_begin_read(&c, q, &q) == -1);                   /* nothing newer */
    rsx_host_end_read(&c, 1);
    assert(c.state[1] == RSX_HOST_FREE);
    /* Several READY: the newest is read and the older ones are freed. */
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 3; i++) { assert(rsx_host_begin_render(&c) == i); }
    for (int i = 0; i < 3; i++) rsx_host_end_render(&c, i, 1, 10 + i);
    assert(rsx_host_begin_read(&c, 0, &q) == 2 && q == 3);
    assert(c.state[0] == RSX_HOST_FREE && c.state[1] == RSX_HOST_FREE && c.state[2] == RSX_HOST_READING);
    /* A reader never takes a frame at or below the one it last showed. */
    rsx_host_end_read(&c, 2);
    assert(rsx_host_begin_render(&c) == 0);
    atomic_store(&c.seq[0], 2); atomic_store(&c.state[0], RSX_HOST_READY);
    assert(rsx_host_begin_read(&c, 3, &q) == -1);
    rsx_host_reset_rendering(&c);
    puts("state machine: ok");
}

int main(void)
{
    @autoreleasepool {
        check_states();
        f_start();

        assert(rsx_metal_backend_init(64, 32, "test") == 0);
        assert(s_host && !s_headless && s_window == nil && NSApp == nil);
        assert(s_host_w == 64 && s_host_h == 32 && s_eng_active);
        assert(atomic_load(&f_ctl->producer_pid) == getpid());

        /* Engine present: the source lands in a host surface. */
        eng_encode_and_commit(solid(64, 32, 0xFF3366CCu));
        assert(f_read_newest(1, NULL) == 0xFF3366CCu);
        puts("engine present: ok");

        /* Nobody reading: no stall, older frames reclaimed. */
        id<MTLTexture> red = solid(64, 32, 0xFFFF0000u);
        for (int i = 0; i < 40; i++) eng_encode_and_commit(red);
        host_drain();
        assert(atomic_load(&f_ctl->published) == 41 && atomic_load(&f_ctl->skipped) == 0);
        assert(atomic_load(&f_ctl->reclaimed) > 0);
        uint64_t seq1 = 0;
        assert(f_read_newest(41, &seq1) == 0xFFFF0000u && seq1 == 41);
        printf("40 unread presents: published 41, reclaimed %llu\n",
               (unsigned long long)atomic_load(&f_ctl->reclaimed));

        /* Guest frames reach the launcher's counter. */
        rsx_metal_backend_note_guest_frame();
        assert(atomic_load(&f_ctl->guest_frames) == 1);

        /* The vtable path: the frame's first pass clears to the guest colour,
         * straight into the surface. Then a display resize. */
        s_eng_active = 0;
        s_clear_argb = 0xFF00FF00u;
        rsx_metal_backend_present();
        assert(f_read_newest(42, NULL) == 0xFF00FF00u);
        s_width = 128; s_height = 64;
        assert(create_depth() == 0);
        rsx_metal_backend_present();
        assert(s_host_w == 128 && s_host_h == 64 && f_w == 128 && f_ctl->generation == 2);
        assert(f_read_newest(43, NULL) == 0xFF00FF00u);
        puts("vtable present and resize: ok");

        /* The launcher closing its window. */
        assert(rsx_metal_backend_pump_messages() == 0);
        atomic_store(&f_ctl->closed, 1);
        rsx_metal_backend_present();
        assert(rsx_metal_backend_pump_messages() == -1);
        atomic_store(&f_ctl->closed, 0);
        puts("closed -> pump -1: ok");

        /* What an exec brings up: shutdown (the old image is gone), a frame
         * it left RENDERING, a fresh init that attaches again. */
        rsx_metal_backend_export_window_state();
        assert(getenv("PS3RECOMP_WINDOW_STATE") == NULL);
        rsx_metal_backend_shutdown();
        assert(!s_host && !s_host_ctl);
        assert(rsx_host_begin_render(f_ctl) >= 0);          /* left behind */
        const int before = atomic_load(&f_attaches);
        assert(rsx_metal_backend_init(128, 64, "test") == 0);
        assert(s_host && atomic_load(&f_attaches) == before + 1);
        for (int i = 0; i < RSX_HOST_SLOTS; i++)
            assert(atomic_load(&f_ctl->state[i]) != RSX_HOST_RENDERING);
        eng_encode_and_commit(solid(128, 64, 0xFF0000FFu));
        uint64_t seq2 = 0;
        assert(f_read_newest(45, &seq2) == 0xFF0000FFu && seq2 == 45 && seq2 > seq1);   /* 44: the present that saw `closed` */
        puts("re-attach after exec: ok");
        rsx_metal_backend_shutdown();
        puts("test_metal_host_surface: ok");
    }
    return 0;
}

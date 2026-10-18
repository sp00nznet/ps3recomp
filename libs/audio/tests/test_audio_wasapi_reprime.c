/* CPU-only WASAPI re-prime contract test. No endpoint or mixer thread is opened.
 * Build with the Windows clang64 toolchain; do not define AUDIO_USE_SDL2. */
#include "../cellAudio.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
int sys_event_queue_push_by_id(uint32_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
uint32_t sys_event_find_queue_by_key(uint64_t key) { (void)key; return 0; }
uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size)
{ (void)key; (void)size; return 0; }

typedef struct {
    IAudioRenderClient iface;
    int gets, releases;
    UINT32 get_frames, release_frames;
    DWORD release_flags;
    HRESULT get_result, release_result;
    BYTE sample[8];
} FakeRender;

static HRESULT STDMETHODCALLTYPE fake_get_buffer(IAudioRenderClient* self, UINT32 frames, BYTE** data)
{
    FakeRender* f = (FakeRender*)self;
    ++f->gets;
    f->get_frames = frames;
    if (FAILED(f->get_result)) return f->get_result;
    *data = f->sample;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE fake_release_buffer(IAudioRenderClient* self, UINT32 frames, DWORD flags)
{
    FakeRender* f = (FakeRender*)self;
    ++f->releases;
    f->release_frames = frames;
    f->release_flags = flags;
    return f->release_result;
}

static IAudioRenderClientVtbl fake_vtbl = {
    .GetBuffer = fake_get_buffer,
    .ReleaseBuffer = fake_release_buffer,
};

static void reset(FakeRender* f)
{
    memset(f, 0, sizeof(*f));
    f->iface.lpVtbl = &fake_vtbl;
    f->get_result = S_OK;
    f->release_result = S_OK;
    s_wasapi_render = &f->iface;
    g_audio_submit_failures = 0;
}

int main(void)
{
    FakeRender f;
    reset(&f);
    assert(audio_backend_pad_silence(384) == 1);
    assert(f.gets == 1 && f.releases == 1);
    assert(f.get_frames == 384 && f.release_frames == 384);
    assert(f.release_flags == AUDCLNT_BUFFERFLAGS_SILENT);
    assert(g_audio_submit_failures == 0);

    reset(&f);
    assert(audio_backend_pad_silence(0) == 0);
    assert(f.gets == 0 && f.releases == 0);
    s_wasapi_render = NULL;
    assert(audio_backend_pad_silence(384) == 0);
    assert(f.gets == 0 && f.releases == 0);

    reset(&f);
    f.get_result = E_FAIL;
    assert(audio_backend_pad_silence(384) == 0);
    assert(f.gets == 1 && f.releases == 0);
    assert(g_audio_submit_failures == 1);

    reset(&f);
    f.release_result = E_FAIL;
    assert(audio_backend_pad_silence(384) == 0);
    assert(f.gets == 1 && f.releases == 1);
    assert(f.release_frames == 384 && f.release_flags == AUDCLNT_BUFFERFLAGS_SILENT);
    assert(g_audio_submit_failures == 1);

    s_wasapi_render = NULL;
    puts("PASS WASAPI silence re-prime success, no-op, GetBuffer failure, ReleaseBuffer failure");
    return 0;
}

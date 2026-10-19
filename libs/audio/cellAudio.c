/*
 * ps3recomp - cellAudio HLE implementation
 *
 * Real audio mixing and output. A background mixing thread reads audio data
 * from each active port's buffer in guest memory, mixes them, and outputs
 * to the host audio device.
 *
 * Backend selection:
 *   - Windows default: WASAPI (via mmdeviceapi)
 *   - Everywhere else / if PS3RECOMP_AUDIO_USE_SDL2 defined: SDL2 audio
 *
 * Define PS3RECOMP_AUDIO_USE_SDL2 to force SDL2 backend on Windows.
 */

#include "../../runtime/memory/vm.h"   /* vm_commit */
#include "cellAudio.h"
#include "audio_pacer.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_read64, vm_write32 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <math.h>

/* Event-queue push/lookup (runtime/syscalls/sys_event.c). Forward-declared
 * rather than including sys_event.h to avoid pulling PPU context/syscall
 * table types into the HLE audio module. */
extern int      sys_event_queue_push_by_id(uint32_t queue_id,
                                            uint64_t source, uint64_t data1,
                                            uint64_t data2,  uint64_t data3);
extern uint32_t sys_event_find_queue_by_key(uint64_t key);
extern uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size);

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_AUDIO_USE_SDL2)
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#elif defined(_WIN32)
  #define AUDIO_BACKEND_SDL2    0
  #define AUDIO_BACKEND_WASAPI  1
#else
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#endif

#if AUDIO_BACKEND_WASAPI
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
  #include <process.h>

  /* Define WASAPI COM GUIDs (avoids needing uuid.lib linkage for these) */
  #ifdef __cplusplus
    #define GUID_SECT
  #else
    #define GUID_SECT
  #endif
  #ifndef DEFINE_AUDIO_GUID
    #ifdef INITGUID
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #else
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #endif
  #endif
  DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

  /* Map to the names used in the code */
  #define IID_IMMDeviceEnumerator  ps3r_IID_IMMDeviceEnumerator
  #define CLSID_MMDeviceEnumerator ps3r_CLSID_MMDeviceEnumerator
  #define IID_IAudioClient         ps3r_IID_IAudioClient
  #define IID_IAudioRenderClient   ps3r_IID_IAudioRenderClient
#endif

#if AUDIO_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* Portable threading */
#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  #include <time.h>
  typedef pthread_t thread_t;
  typedef pthread_mutex_t mutex_t;
  #define mutex_init(m)    pthread_mutex_init(m, NULL)
  #define mutex_destroy(m) pthread_mutex_destroy(m)
  #define mutex_lock(m)    pthread_mutex_lock(m)
  #define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Per-port audio buffer allocated on the host side.
 * In a full emulator these would be in guest VM memory; here we allocate
 * host buffers and expose their addresses through portAddr/readIndexAddr. */
#define AUDIO_PORT_BUF_MAX  (CELL_AUDIO_BLOCK_32 * CELL_AUDIO_BLOCK_SAMPLES * CELL_AUDIO_PORT_8CH)

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;        /* host audio buffer (float samples) */
    u32                  buf_size;       /* buffer size in bytes */
    u64                  read_index;     /* current read position (block index) */
    u64                  write_index;    /* where the game is writing */
    /* For address reporting to guest */
    u64                  port_addr;      /* guest-visible buffer address */
    u64                  read_idx_addr;  /* guest-visible read index address */
} AudioPortSlot;

static int            s_audio_initialized = 0;
static AudioPortSlot  s_ports[CELL_AUDIO_PORT_MAX];

/* Event queue notification */
typedef struct {
    int  in_use;
    u64  key;
} AudioNotifySlot;
static AudioNotifySlot s_notify_queues[CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES];

/* Timestamp anchor for cellAudioGetPortTimestamp (microseconds, set at init) */
static u64            s_audio_start_us = 0;

/* Mixing thread */
static unsigned long long g_audio_dropped;   /* frames the device had no room for */
static unsigned long long g_audio_submit_failures, g_audio_submitted;
/* Device fill diagnostics: blocks that found the device queue empty (it ran
 * dry: the engine played zeros) and blocks produced more than one engine
 * period after their deadline (late submissions). */
static unsigned long long g_audio_underruns, g_audio_late, g_audio_notify_full, g_audio_reprimes;
static atomic_int    s_mix_thread_running = 0;
static thread_t      s_mix_thread;
#ifndef _WIN32
/* Is s_mix_thread a thread that exists and has not been joined? The Win32
 * branch asks that of the HANDLE itself, which it nulls after closing;
 * pthread_t has no such value, so carry the flag. */
static int           s_mix_thread_live = 0;
#endif
static mutex_t       s_audio_mutex;

/* Output mix buffer (stereo, one block worth) */
static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 2];

/* ---------------------------------------------------------------------------
 * Host audio output backend
 * -----------------------------------------------------------------------*/

#if AUDIO_BACKEND_SDL2

static SDL_AudioDeviceID s_sdl_audio_dev = 0;

static int audio_backend_init(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[cellAudio] SDL audio init failed: %s\n", SDL_GetError());
            return -1;
        }
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = CELL_AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = CELL_AUDIO_BLOCK_SAMPLES;
    want.callback = NULL; /* We'll use SDL_QueueAudio */

    s_sdl_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_sdl_audio_dev == 0) {
        printf("[cellAudio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(s_sdl_audio_dev, 0); /* Start playback */
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_sdl_audio_dev) {
        SDL_CloseAudioDevice(s_sdl_audio_dev);
        s_sdl_audio_dev = 0;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (s_sdl_audio_dev) {
        if (SDL_QueueAudio(s_sdl_audio_dev, stereo_samples,
                          num_samples * 2 * sizeof(float)) < 0) {
            ++g_audio_submit_failures;
            if (g_audio_submit_failures <= 4)
                fprintf(stderr, "[cellAudio] SDL_QueueAudio failed: %s\n", SDL_GetError());
        } else g_audio_submitted += num_samples;
    }
}

/* SDL queues without bound: keep about four blocks ahead of the device. */
static int audio_backend_room(void)
{
    if (!s_sdl_audio_dev) return -1;
    return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float)) <
           CELL_AUDIO_BLOCK_SAMPLES * 4 ? CELL_AUDIO_BLOCK_SAMPLES : 0;
}

static void audio_backend_wait(void) { SDL_Delay(1); }
/* Fill steering is WASAPI-only; SDL keeps its queue-depth heuristic. */
static u32 audio_backend_buffer_frames(void) { return 0; }
static u32 audio_backend_period_frames(void) { return 0; }
static u32 audio_backend_target_frames(void) { return 0; }
static int audio_backend_pad_silence(u32 frames) { (void)frames; return 0; }

static u32 audio_backend_queued_samples(void)
{
    if (s_sdl_audio_dev) {
        return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float));
    }
    return 0;
}

#endif /* AUDIO_BACKEND_SDL2 */

#if AUDIO_BACKEND_WASAPI

static IAudioClient*        s_wasapi_client = NULL;
static IAudioRenderClient*  s_wasapi_render = NULL;
static HANDLE               s_wasapi_event  = NULL;
static UINT32               s_wasapi_buf_frames = 0;
static UINT32               s_wasapi_period_frames = 0;
static UINT32               s_wasapi_target_frames = 0;
static int audio_paced(void) { return getenv("AUDIO_DEVICE_BURST") == NULL; }

static int audio_backend_init(void)
{
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        printf("[cellAudio] CoInitializeEx failed: 0x%08lX\n", hr);
        return -1;
    }

    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to create device enumerator: 0x%08lX\n", hr);
        return -1;
    }

    IMMDevice* device = NULL;
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender,
                                                      eConsole, &device);
    enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get default audio endpoint: 0x%08lX\n", hr);
        return -1;
    }

    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void**)&s_wasapi_client);
    device->lpVtbl->Release(device);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to activate audio client: 0x%08lX\n", hr);
        return -1;
    }

    WAVEFORMATEX wfx;
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = CELL_AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    /* Event-driven shared stream with a 100 ms buffer: room for the standing
     * fill (audio_fill_target) plus catch-up after a late wake. The old 20 ms
     * buffer could hold little more than one engine period. */
    REFERENCE_TIME buf_duration = 1000000; /* 100 ms in 100 ns units */
    s_wasapi_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    hr = s_wasapi_client->lpVtbl->Initialize(
        s_wasapi_client,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        buf_duration, 0, &wfx, NULL);

    if (FAILED(hr)) {
        /* Fallback: try without event callback */
        hr = s_wasapi_client->lpVtbl->Initialize(
            s_wasapi_client,
            AUDCLNT_SHAREMODE_SHARED,
            0, buf_duration, 0, &wfx, NULL);
        if (FAILED(hr)) {
            printf("[cellAudio] WASAPI Initialize failed: 0x%08lX\n", hr);
            s_wasapi_client->lpVtbl->Release(s_wasapi_client);
            s_wasapi_client = NULL;
            CloseHandle(s_wasapi_event);
            s_wasapi_event = NULL;
            return -1;
        }
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    } else {
        s_wasapi_client->lpVtbl->SetEventHandle(s_wasapi_client, s_wasapi_event);
    }

    s_wasapi_client->lpVtbl->GetBufferSize(s_wasapi_client, &s_wasapi_buf_frames);
    {
        REFERENCE_TIME default_period = 0, minimum_period = 0;
        if (FAILED(s_wasapi_client->lpVtbl->GetDevicePeriod(s_wasapi_client, &default_period, &minimum_period)) ||
            default_period <= 0)
            default_period = 100000; /* 10 ms */
        s_wasapi_period_frames = (UINT32)((default_period * CELL_AUDIO_SAMPLE_RATE + 9999999) / 10000000);
        s_wasapi_target_frames = audio_fill_target(s_wasapi_period_frames, s_wasapi_buf_frames);
    }

    hr = s_wasapi_client->lpVtbl->GetService(
        s_wasapi_client, &IID_IAudioRenderClient, (void**)&s_wasapi_render);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get render client: 0x%08lX\n", hr);
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
        return -1;
    }

    /* A device callback is an engine quantum, not a guest block clock.
     * Prime the standing fill with silence so the producer can run at 48 kHz
     * with a margin of more than one engine pass. This is startup latency
     * only; the pacer's trim then holds the fill there. */
    fprintf(stderr, "[cellAudio] WASAPI shared: buffer %u frames, period %u, standing fill %u (%.1f ms)\n",
            (unsigned)s_wasapi_buf_frames, (unsigned)s_wasapi_period_frames, (unsigned)s_wasapi_target_frames,
            s_wasapi_target_frames * 1000.0 / CELL_AUDIO_SAMPLE_RATE);
    if (audio_paced()) {
        UINT32 prime = s_wasapi_target_frames;
        if (prime > s_wasapi_buf_frames) prime = s_wasapi_buf_frames;
        BYTE* buffer = NULL;
        hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, prime, &buffer);
        if (SUCCEEDED(hr))
            hr = s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, prime, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(hr)) { fprintf(stderr, "[cellAudio] prime failed: 0x%08lX\n", hr); return -1; }
    }
    hr = s_wasapi_client->lpVtbl->Start(s_wasapi_client);
    if (FAILED(hr)) { fprintf(stderr, "[cellAudio] Start failed: 0x%08lX\n", hr); return -1; }
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Stop(s_wasapi_client);
    }
    if (s_wasapi_render) {
        s_wasapi_render->lpVtbl->Release(s_wasapi_render);
        s_wasapi_render = NULL;
    }
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
    }
    if (s_wasapi_event) {
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (!s_wasapi_render) return;

    UINT32 padding = 0;
    HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr) || padding > s_wasapi_buf_frames) {
        ++g_audio_submit_failures;
        if (g_audio_submit_failures <= 4)
            fprintf(stderr, "[cellAudio] submit padding failed: 0x%08lX\n", hr);
        return;
    }

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available) {
        g_audio_dropped += num_samples - available;
        num_samples = available;
    }
    if (num_samples == 0) return;

    BYTE* buf = NULL;
    hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        memcpy(buf, stereo_samples, num_samples * 2 * sizeof(float));
        hr = s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, num_samples, 0);
        if (SUCCEEDED(hr)) { g_audio_submitted += num_samples; return; }
    }
    ++g_audio_submit_failures;
    if (g_audio_submit_failures <= 4)
        fprintf(stderr, "[cellAudio] render submission failed: 0x%08lX\n", hr);
}

/* Frames the device can take right now; -1 with no device open. */
static int audio_backend_room(void)
{
    if (!s_wasapi_client) return -1;
    UINT32 padding = 0;
    const HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr)) {
        /* Device gone (AUDCLNT_E_DEVICE_INVALIDATED on a default-device
         * change, sleep, ...): pace on the clock rather than spin. */
        static int _n = 0; if (_n++ < 4)
            fprintf(stderr, "[cellAudio] GetCurrentPadding failed 0x%08lX -- clock pacing%c", (unsigned long)hr, 10);
        return -1;
    }
    return (int)(s_wasapi_buf_frames - padding);
}

/* Queue `frames` of silence behind what the device holds (see the mixer's
 * stall handling). */
static int audio_backend_pad_silence(u32 frames)
{
    BYTE* buffer = NULL;
    if (!s_wasapi_render || !frames) return 0;
    HRESULT hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, frames, &buffer);
    if (SUCCEEDED(hr))
        hr = s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, frames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (SUCCEEDED(hr)) return 1;
    ++g_audio_submit_failures;
    if (g_audio_submit_failures <= 4)
        fprintf(stderr, "[cellAudio] re-prime failed: 0x%08lX\n", hr);
    return 0;
}
static u32 audio_backend_buffer_frames(void) { return s_wasapi_buf_frames; }
static u32 audio_backend_period_frames(void) { return s_wasapi_period_frames; }
static u32 audio_backend_target_frames(void) { return s_wasapi_target_frames; }

/* Block until the device has consumed some of its buffer. */
static void audio_backend_wait(void)
{
    if (s_wasapi_event) WaitForSingleObject(s_wasapi_event, 10);
    else Sleep(1);
}


#endif /* AUDIO_BACKEND_WASAPI */

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Guest PCM is BIG-ENDIAN float32; the port buffer is a raw vm_base view, so
 * every sample must be byte-swapped before use. Reading it as a host float
 * turned the whole mix into denormal noise -- silence after clipping (LBP
 * had a verified end-to-end pipeline with no audible output). */
static inline float ld_be_f32(const float* p)
{
    u32 v;
    memcpy(&v, p, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

#if AUDIO_BACKEND_WASAPI
/* Optional same-slot PCM evidence. Snapshots may overlap guest writes; the
 * timestamps bound that observation rather than claiming an atomic block. */
static int s_pcm_probe_enabled;
static unsigned s_pcm_probe_remaining;
static const char *s_pcm_probe_request;
static unsigned s_pcm_probe_poll;
static int s_pcm_probe_armed;
static uint64_t s_pcm_probe_frequency;
static struct {
    uint64_t begin_us,end_us,read_index;
    unsigned valid,port,slot,nonzero,nonfinite;
    float peak;
} s_pcm_probe;
static uint64_t audio_pcm_probe_now(void)
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    const uint64_t q=(uint64_t)t.QuadPart,f=s_pcm_probe_frequency;
    return (q/f)*1000000ULL+(q%f)*1000000ULL/f;
}
static void audio_pcm_probe_slot(unsigned p, const AudioPortSlot *port,
                                  unsigned slot, const float *src, unsigned count)
{
    if (!s_pcm_probe_enabled || !s_pcm_probe_remaining || s_pcm_probe.valid) return;
    /* Optional sentinel delays the bounded snapshot until gameplay. This is
     * diagnostic-only and never removes a caller-owned request file. */
    if (!s_pcm_probe_armed) {
        if (s_pcm_probe_poll++ % 188u) return;
        if (GetFileAttributesA(s_pcm_probe_request)==INVALID_FILE_ATTRIBUTES) return;
        s_pcm_probe_armed=1;
    }
    --s_pcm_probe_remaining;
    s_pcm_probe.valid=1; s_pcm_probe.port=p; s_pcm_probe.slot=slot;
    s_pcm_probe.read_index=port->read_index;
    s_pcm_probe.begin_us=audio_pcm_probe_now();
    for (unsigned i=0;i<count;++i) {
        const float f=ld_be_f32(src+i);
        if (f!=0.0f) ++s_pcm_probe.nonzero;
        if (!isfinite(f)) ++s_pcm_probe.nonfinite;
        else if (fabsf(f)>s_pcm_probe.peak) s_pcm_probe.peak=fabsf(f);
    }
    s_pcm_probe.end_us=audio_pcm_probe_now();
}
#endif

/* AUDIO_BLOCK_TRACE only: how many blocks after the read cursor of the first
 * running port already hold PCM -- the game's lead over the device. Zero
 * means the next block will be read before the game has written it. */
static unsigned audio_ring_lead(void)
{
    unsigned lead = 0;
    mutex_lock(&s_audio_mutex);
    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        if (!port->in_use || !port->running || !port->buffer) continue;
        const u32 nch = (u32)port->param.nChannel, nblock = (u32)port->param.nBlock;
        for (u32 b = 0; b + 1 < nblock; b++) {
            const float* bp = port->buffer + ((port->read_index + b) % nblock) * CELL_AUDIO_BLOCK_SAMPLES * nch;
            int nz = 0;
            for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES * nch; s += 8)
                if (bp[s] != 0.0f) { nz = 1; break; }
            if (!nz) break;
            ++lead;
        }
        break;
    }
    mutex_unlock(&s_audio_mutex);
    return lead;
}

/* Mix one block from all active ports into s_mix_buffer (stereo float) */
static void audio_mix_one_block(void)
{
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));
#if AUDIO_BACKEND_WASAPI
    if (s_pcm_probe_enabled) memset(&s_pcm_probe,0,sizeof(s_pcm_probe));
#endif

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        /* Diagnostic: a port that HOLDS PCM but was never started is inaudible
         * by design -- flag it once so "no PortStart" vs "no data" is decidable
         * from the log (LBP's Bink movie audio port sat exactly there). */
        if (port->in_use && !port->running && port->buffer) {
            static u8 warned[CELL_AUDIO_PORT_MAX];
            if (!warned[p]) {
                u32 probe = (u32)(port->param.nBlock * CELL_AUDIO_BLOCK_SAMPLES *
                                  port->param.nChannel);
                int nz = 0;
                for (u32 s = 0; s < probe; s += 64) if (port->buffer[s] != 0.0f) { nz = 1; break; }
                if (nz) {
                    warned[p] = 1;
                    fprintf(stderr, "[cellAudio] port %d HAS DATA but never started"
                            " -- game withheld PortStart\n", p);
                }
            }
        }
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        float level = port->param.level;
        if (level <= 0.0f) level = 1.0f;
        if (level > 1.0f) level = 1.0f;

        /* Read one block at the current read_index */
        u32 block_idx = (u32)(port->read_index % nblock);
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        float* src = port->buffer + block_offset;
#if AUDIO_BACKEND_WASAPI
        audio_pcm_probe_slot((unsigned)p,port,block_idx,src,CELL_AUDIO_BLOCK_SAMPLES*nch);
#endif

        /* AUDIO_PEAK diag: which blocks of this port's WHOLE ring hold data,
         * vs which block the read cursor is on -- distinguishes "writer never
         * writes" from "writer and reader cycle out of phase". */
        { static int s_pk = -1; if (s_pk < 0) s_pk = getenv("AUDIO_PEAK") ? 1 : 0;
          if (s_pk) { static _Thread_local unsigned _c;
            if ((++_c % 400) == 0) {
                char map[36]; u32 b;
                for (b = 0; b < nblock && b < 32; b++) {
                    const float* bp = port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch;
                    int nz = 0;
                    for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 16)
                        if (bp[s2] != 0.0f) { nz = 1; break; }
                    map[b] = nz ? '#' : '.';
                }
                map[b] = 0;
                fprintf(stderr, "[audio-ring] port %d ridx=%llu blk=%u ring=[%s]\n",
                        p, (unsigned long long)port->read_index, block_idx, map);
            } } }

        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float left, right;
            if (nch >= 2) {
                left  = ld_be_f32(&src[s * nch + 0]) * level;
                right = ld_be_f32(&src[s * nch + 1]) * level;
            } else {
                /* Mono: duplicate to both channels */
                left = right = ld_be_f32(&src[s]) * level;
            }

            /* If 7.1, mix center and other channels into stereo */
            if (nch == 8) {
                float center = ld_be_f32(&src[s * 8 + 2]) * level * 0.707f;
                float lfe    = ld_be_f32(&src[s * 8 + 3]) * level * 0.5f;
                float rl     = ld_be_f32(&src[s * 8 + 4]) * level * 0.5f;
                float rr     = ld_be_f32(&src[s * 8 + 5]) * level * 0.5f;
                float sl     = ld_be_f32(&src[s * 8 + 6]) * level * 0.3f;
                float sr     = ld_be_f32(&src[s * 8 + 7]) * level * 0.3f;
                left  += center + lfe + rl + sl;
                right += center + lfe + rr + sr;
            }

            s_mix_buffer[s * 2 + 0] += left;
            s_mix_buffer[s * 2 + 1] += right;
        }

        /* Firmware clears a ring-buffer block after consuming it. Besides
         * preventing stale PCM from replaying when a producer stalls, the
         * cleared block is an observable consumption signal for producers. */
        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));

        /* Keep the monotonic counter for tags/timestamps, but publish the
         * ring slot to the guest. SurMixer multiplies this value by the block
         * size directly; an unbounded counter walks beyond the audio buffer. */
        port->read_index++;
        if (port->read_idx_addr)
            vm_write64((u32)port->read_idx_addr, port->read_index % nblock);
    }

    mutex_unlock(&s_audio_mutex);

    /* Clip to [-1.0, 1.0] */
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
        if (s_mix_buffer[i] > 1.0f) s_mix_buffer[i] = 1.0f;
        if (s_mix_buffer[i] < -1.0f) s_mix_buffer[i] = -1.0f;
    }
}

/* ---------------------------------------------------------------------------
 * Mixing thread
 * -----------------------------------------------------------------------*/

static void audio_notify_event_queues(void)
{
    /* Push the audio-period event (data1 = CELL_AUDIO_EVENT_MIX = 0) to each
     * registered notify queue, so the game's audio loop (blocked on
     * sys_event_queue_receive) wakes once per block and writes the next one.
     * Resolve the queue by its ipc_key (set via cellAudioSetNotifyEventQueue). */
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) continue;
        uint32_t qid = sys_event_find_queue_by_key(s_notify_queues[i].key);
        /* A full queue means the game's audio thread is still behind on
         * earlier blocks: count it (the block it misses plays as silence). */
        if (qid && sys_event_queue_push_by_id(qid, s_notify_queues[i].key,
                                              0 /*CELL_AUDIO_EVENT_MIX*/, 0, 0) == -1)
            ++g_audio_notify_full;
    }
}

#ifdef _WIN32
/* A per-thread multimedia reservation and per-thread high resolution timer
 * keep audio scheduling independent of video activity and process-wide timer
 * resolution. Neither changes the default device or global audio settings. */
static void audio_wait_clock(HANDLE timer, uint64_t ticks, uint64_t frequency)
{
    if (!ticks) return;
    if (timer) {
        LARGE_INTEGER relative;
        relative.QuadPart = -(LONGLONG)((ticks * 10000000ULL + frequency - 1) / frequency);
        if (SetWaitableTimer(timer, &relative, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, 10);
            return;
        }
    }
    Sleep(1);
}

static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    /* Pace to the device clock. The PS3 consumes one block every 5.33 ms;
     * the old Sleep(2)/Sleep(5) loop keyed on a queue depth the 1056-frame
     * WASAPI buffer can never reach, so it mixed ~225 blocks/s and the device
     * dropped the surplus: 20% of the audio thrown away, and a guest mixer
     * (GH3's FMOD) that could not keep ahead of the read index played gaps.
     * With no device, pace on the performance counter instead. */
    LARGE_INTEGER qf, q0; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    unsigned long long blocks = 0;
    audio_pacer pacer;
    audio_pacer_init(&pacer, (uint64_t)q0.QuadPart, (uint64_t)qf.QuadPart);
    const int paced = getenv("AUDIO_DEVICE_BURST") == NULL;
    HANDLE timer = NULL, mmcss = NULL;
    HMODULE avrt = NULL;
    typedef HANDLE (WINAPI *mmcss_begin_fn)(LPCWSTR, LPDWORD);
    typedef BOOL (WINAPI *mmcss_end_fn)(HANDLE);
    mmcss_end_fn mmcss_end = NULL;
    if (paced) {
        timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
        avrt = LoadLibraryW(L"avrt.dll");
        if (avrt) {
            mmcss_begin_fn begin = (mmcss_begin_fn)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
            mmcss_end = (mmcss_end_fn)GetProcAddress(avrt, "AvRevertMmThreadCharacteristics");
            DWORD task_index = 0;
            if (begin && mmcss_end) mmcss = begin(L"Audio", &task_index);
        }
        fprintf(stderr, "[cellAudio] paced 48kHz, timer=%s, MMCSS Audio=%s\n",
                timer ? "waitable" : "Sleep fallback", mmcss ? "enabled" : "unavailable");
    }
    audio_fill fill = {0};
    fill.target = audio_backend_target_frames();
    const u32 device_frames = audio_backend_buffer_frames();
    const uint64_t period_ticks = (uint64_t)qf.QuadPart * audio_backend_period_frames() / CELL_AUDIO_SAMPLE_RATE;
    unsigned long long reported_underruns = 0; ULONGLONG report_at = GetTickCount64() + 60000;
    const char* trace_path = getenv("AUDIO_BLOCK_TRACE");
    FILE* trace = trace_path ? fopen(trace_path, "wb") : NULL;
    const char *pcm_probe=getenv("FOLKLORE_AUDIO_PCM_TRACE");
    s_pcm_probe_enabled=trace && pcm_probe && strcmp(pcm_probe,"1")==0;
    s_pcm_probe_remaining=12000; /* At most64 seconds of48kHz/256-frame blocks. */
    s_pcm_probe_request=getenv("FOLKLORE_AUDIO_PCM_TRACE_REQUEST");
    s_pcm_probe_armed=!s_pcm_probe_request || !*s_pcm_probe_request;
    s_pcm_probe_poll=0;
    memset(&s_pcm_probe,0,sizeof(s_pcm_probe));
    s_pcm_probe_frequency=(uint64_t)qf.QuadPart;
    if (trace) fprintf(trace, "block,qpc_us,room,peak,silent,dropped,submitted,submit_failures,rebases,mix_us,capture_submit_us,previous_tail_us,padding,target,late_us,underruns,late,trim_us,notify_full,lead,reprimes,pcm_valid,pcm_port,pcm_slot,pcm_read_index,pcm_begin_us,pcm_end_us,pcm_nonzero,pcm_nonfinite,pcm_peak\n");
    uint64_t late_ticks = 0;
    unsigned long long seen_rebases = 0;
    uint64_t previous_tail_us = 0;
    LARGE_INTEGER trace_now = {0};
    while (s_mix_thread_running) {
        const int room = audio_backend_room();
        if (room >= 0 && room < CELL_AUDIO_BLOCK_SAMPLES) {
            if (paced) audio_wait_clock(timer, (uint64_t)qf.QuadPart / 1000, (uint64_t)qf.QuadPart);
            else audio_backend_wait();
            continue;
        }
        if (room < 0 || paced) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            const uint64_t scheduled = pacer.epoch + audio_pacer_offset(&pacer);
            if (!audio_pacer_ready(&pacer, (uint64_t)now.QuadPart)) {
                uint64_t due = audio_pacer_deadline(&pacer);
                audio_wait_clock(timer, due > (uint64_t)now.QuadPart ? due - now.QuadPart : 1, (uint64_t)qf.QuadPart);
                continue;
            }
            late_ticks = (uint64_t)now.QuadPart > scheduled ? (uint64_t)now.QuadPart - scheduled : 0;
            if (period_ticks && late_ticks > period_ticks) ++g_audio_late;
            audio_pacer_consumed(&pacer, (uint64_t)now.QuadPart);
        }
        u32 padding = 0;
        if (room >= 0 && device_frames) {
            padding = device_frames > (u32)room ? device_frames - (u32)room : 0;
            if (padding == 0 && blocks > 0) ++g_audio_underruns;
            /* Recover only when a >50 ms pacer stall also left the device
             * empty. A paused endpoint can retain audio during a thread
             * stall: adding silence there would create a new gap. Restore
             * the standing fill after a confirmed underrun, at
             * the gap that already exists, instead of trimming it back over
             * ~30 s of near-empty queue and repeated underruns. No guest
             * block is dropped, replaced or reordered. */
            if (pacer.rebases != seen_rebases) {
                seen_rebases = pacer.rebases;
                if (paced && fill.target && padding == 0) {
                    if (audio_backend_pad_silence(fill.target)) {
                        padding = fill.target;
                        ++g_audio_reprimes;
                    }
                }
            }
            if (paced && fill.target) audio_fill_observe(&fill, &pacer, padding);
        }
        blocks++;
        /* Mix and submit one block */
        LARGE_INTEGER mix_begin = {0}, mix_end = {0};
        if (trace) QueryPerformanceCounter(&mix_begin);
        audio_mix_one_block();
        if (trace) QueryPerformanceCounter(&mix_end);
        /* AUDIO_PEAK=1: report the mixed block's peak amplitude periodically so
         * "is any port producing sound" is answerable (LBP Bink movie audio). */
        { static int _ap = -1; if (_ap < 0) _ap = getenv("AUDIO_PEAK") ? 1 : 0;
          if (_ap) { static unsigned _n = 0; float pk = 0.0f;
            for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
                float a = s_mix_buffer[i]; if (a < 0) a = -a; if (a > pk) pk = a; }
            if ((++_n % 200) == 0 || (pk > 0.001f && _n < 40))
                fprintf(stderr, "[audio-peak] block#%u peak=%.4f\n", _n, pk); } }
        /* AUDIO_WAV=<file>: append the final mix as raw f32le stereo 48 kHz
         * (ffmpeg -f f32le -ar 48000 -ac 2 -i <file>). */
        { static FILE* _wf = (FILE*)-1;
          if (_wf == (FILE*)-1) { const char* e = getenv("AUDIO_WAV"); _wf = e ? fopen(e, "wb") : NULL; }
          if (_wf) fwrite(s_mix_buffer, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * 2, _wf); }
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        if (trace) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            trace_now = now;
            float peak = 0.0f;
            for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; ++i) {
                float a = fabsf(s_mix_buffer[i]); if (a > peak) peak = a;
            }
            fprintf(trace, "%llu,%llu,%d,%.9g,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%llu,%llu,%llu,%lld,%llu,%u,%llu", blocks,
                (unsigned long long)(now.QuadPart / qf.QuadPart) * 1000000ULL +
                (unsigned long long)(now.QuadPart % qf.QuadPart) * 1000000ULL / qf.QuadPart,
                room, peak, peak == 0.0f, g_audio_dropped,
                g_audio_submitted, g_audio_submit_failures, (unsigned long long)pacer.rebases,
                (unsigned long long)(mix_end.QuadPart-mix_begin.QuadPart)*1000000ULL/qf.QuadPart,
                (unsigned long long)(now.QuadPart-mix_end.QuadPart)*1000000ULL/qf.QuadPart,
                (unsigned long long)previous_tail_us, (unsigned)padding, (unsigned)fill.target,
                (unsigned long long)(late_ticks*1000000ULL/qf.QuadPart), g_audio_underruns, g_audio_late,
                (long long)(fill.trim_ticks*1000000LL/qf.QuadPart), g_audio_notify_full, audio_ring_lead(), g_audio_reprimes);
            fprintf(trace,",%u,%u,%u,%llu,%llu,%llu,%u,%u,%.9g\n",
                s_pcm_probe.valid,s_pcm_probe.port,s_pcm_probe.slot,
                (unsigned long long)s_pcm_probe.read_index,
                (unsigned long long)s_pcm_probe.begin_us,(unsigned long long)s_pcm_probe.end_us,
                s_pcm_probe.nonzero,s_pcm_probe.nonfinite,s_pcm_probe.peak);
        }
        /* One line a minute, only when the device ran dry in that minute. */
        if (GetTickCount64() >= report_at) {
            if (g_audio_underruns != reported_underruns)
                fprintf(stderr, "[cellAudio] device ran dry %llu times in the last minute (fill target %u frames, %llu late blocks so far)\n",
                        g_audio_underruns - reported_underruns, (unsigned)fill.target, g_audio_late);
            reported_underruns = g_audio_underruns; report_at = GetTickCount64() + 60000;
        }
        /* AUDIO_RATE=1: blocks mixed vs real time, and frames dropped. */
        { static int on = -1; if (on < 0) on = getenv("AUDIO_RATE") ? 1 : 0;
          if (on) { static ULONGLONG t0; static unsigned n, z; static unsigned long long d0;
            ULONGLONG now = GetTickCount64(); if (!t0) t0 = now; n++;
            { int nz = 0; for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) if (s_mix_buffer[i] != 0.0f) { nz = 1; break; }
              if (!nz) z++; }
            if (now - t0 >= 5000) {
                fprintf(stderr, "[audio-rate] %.1f blocks/s (real time 187.5), silent %.1f%%, dropped %.0f frames/s, underruns %llu, late %llu, notify full %llu, fill %u/%u, trim %+.0f us, device buf %u%c",
                        n * 1000.0 / (now - t0), n ? 100.0 * z / n : 0.0, (g_audio_dropped - d0) * 1000.0 / (now - t0),
                        g_audio_underruns, g_audio_late, g_audio_notify_full, (unsigned)padding, (unsigned)fill.target,
                        fill.trim_ticks * 1e6 / (double)qf.QuadPart,
                #if AUDIO_BACKEND_WASAPI
                        (unsigned)s_wasapi_buf_frames, 10);
#else
                        0u, 10);
#endif
                n = z = 0; d0 = g_audio_dropped; t0 = now; } } }

        /* Notify event queues */
        audio_notify_event_queues();
        if (trace) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            previous_tail_us = (uint64_t)(now.QuadPart-trace_now.QuadPart)*1000000ULL/qf.QuadPart;
        }
    }

    if (trace) fclose(trace);
    if (timer) CloseHandle(timer);
    if (mmcss) mmcss_end(mmcss);
    if (avrt) FreeLibrary(avrt);
    printf("[cellAudio] Mixing thread stopped\n");
    return 0;
}
#else
static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    while (s_mix_thread_running) {
        audio_mix_one_block();
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        audio_notify_event_queues();

        u32 queued = audio_backend_queued_samples();
        if (queued > CELL_AUDIO_BLOCK_SAMPLES * 4) {
            usleep(5000);
        } else {
            usleep(2000);
        }
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return NULL;
}
#endif

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;

#ifdef _WIN32
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
#else
    if (pthread_create(&s_mix_thread, NULL, audio_mix_thread_func, NULL) != 0) {
        s_mix_thread_running = 0;
        return -1;
    }
    s_mix_thread_live = 1;
#endif
    return 0;
}

static int audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
#ifdef _WIN32
    if (s_mix_thread) {
        if (WaitForSingleObject(s_mix_thread, 2000) != WAIT_OBJECT_0) {
            fprintf(stderr, "[cellAudio] Mixer has not exited; retaining backend and port state\n");
            return -1;
        }
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
#else
    /* pthread_t has no null value to test, and joining an unset one, or
     * joining the same one twice, is undefined rather than an error the way
     * WaitForSingleObject on a null HANDLE is. */
    if (s_mix_thread_live) {
        if (pthread_join(s_mix_thread, NULL) != 0) return -1;
        s_mix_thread_live = 0;
    }
#endif
    return 0;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioInit(void)
{
    /* PS3_NO_AUDIO=1: report that the audio library is unavailable. Middleware
     * that drives its mixer through SPU jobs will otherwise block the whole boot
     * waiting on work we cannot yet complete; failing here lets a title take its
     * silent path and keep going. Diagnostic first, workaround second. */
    if (getenv("PS3_NO_AUDIO")) {
        printf("[cellAudio] PS3_NO_AUDIO -- reporting audio unavailable\n");
        return (s32)CELL_AUDIO_ERROR_NOT_INIT;
    }
    printf("[cellAudio] Init()\n");

    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    memset(s_notify_queues, 0, sizeof(s_notify_queues));
    g_audio_dropped = g_audio_submit_failures = g_audio_submitted = 0;
    mutex_init(&s_audio_mutex);

    if (audio_backend_init() < 0) {
        fprintf(stderr, "[cellAudio] Audio backend initialization failed\n");
        audio_backend_shutdown();
        mutex_destroy(&s_audio_mutex);
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }


#ifdef _WIN32
    { LARGE_INTEGER f, c;
      QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
      s_audio_start_us = (u64)(c.QuadPart / f.QuadPart) * 1000000ULL +
          (u64)(c.QuadPart % f.QuadPart) * 1000000ULL / (u64)f.QuadPart; }
#else
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      s_audio_start_us = (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL; }
#endif
    if (audio_start_mix_thread() < 0) {
        fprintf(stderr, "[cellAudio] Could not start mixing thread\n");
        audio_backend_shutdown();
        mutex_destroy(&s_audio_mutex);
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }

    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (audio_stop_mix_thread() < 0)
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    audio_backend_shutdown();

    /* Port buffers live in the guest vm_base arena (bump-allocated, not host
     * malloc) -- just drop the references; never free() them. */
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].buffer = NULL;
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }

    mutex_destroy(&s_audio_mutex);
    s_audio_initialized = 0;
    return CELL_OK;
}

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /* param / portNum are GUEST addresses; translate, and read the BE u64 param
     * fields via vm_read64 (a raw param->nChannel faults / is host-endian). */
    uint32_t param_ea   = (uint32_t)(uintptr_t)param;
    uint32_t portNum_ea = (uint32_t)(uintptr_t)portNum;
    if (!param_ea || !portNum_ea)
        return CELL_AUDIO_ERROR_PARAM;

    u64 nch  = vm_read64(param_ea + 0);   /* nChannel */
    u64 nblk = vm_read64(param_ea + 8);   /* nBlock   */
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           (unsigned long long)nch, (unsigned long long)nblk);

    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);

    /* Find a free port slot */
    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            found = (s32)i;
            break;
        }
    }

    if (found < 0) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_FULL;
    }

    AudioPortSlot* port = &s_ports[found];
    port->in_use  = 1;
    port->running = 0;
    /* Store the decoded (host-endian) param values, not the raw BE guest bytes. */
    memset(&port->param, 0, sizeof(port->param));
    port->param.nChannel = nch;
    port->param.nBlock   = nblk;
    port->read_index  = 0;
    port->write_index = 0;

    /* Allocate the audio buffer in GUEST memory so portAddr/readIndexAddr are
     * real guest addresses (the game's mixer -- FMOD here -- writes PCM there
     * and validates them as 1 MB-aligned guest pointers; a host pointer trips
     * its "invalid parameter" check). Bump from a free main-memory window; the
     * host-side `buffer` is just the vm_base view of the same bytes. */
    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);

    /* Keep audio outside sys_memory (0x40000000..0x50000000), the HLE heap
     * (0x50000000..0x58000000), and sys_vm (0x60000000..0x70000000).
     * Reserve two 1-MB pages per port so reopening reuses its own storage. */
    u32 guest_buf = 0x58000000u + (u32)found * 0x200000u;
    u32 guest_ridx = guest_buf + 0x100000u;
    if (vm_commit(guest_buf, 0x200000u) != CELL_OK) {
        port->in_use = 0;
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }

    port->buffer = (float*)(vm_base + guest_buf);     /* host view of guest buffer */
    memset(port->buffer, 0, port->buf_size);
    vm_write64(guest_ridx, 0);                        /* read index counter (u64, see below) */

    port->port_addr     = guest_buf;
    port->read_idx_addr = guest_ridx;

    vm_write32(portNum_ea, (u32)found);   /* guest out-param */

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    mutex_lock(&s_audio_mutex);

    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* buffer points INTO the guest vm_base arena (reserved per port in PortOpen),
     * not a host malloc -- do NOT free() it (that corrupts the host heap).
     * The guest window is reclaimed wholesale when vm_base is released. */
    s_ports[portNum].buffer = NULL;

    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 1;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioPortStop(u32 portNum)
{
    printf("[cellAudio] PortStop(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, u64* tag)
{
    uint32_t tag_ea = (uint32_t)(uintptr_t)tag;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!tag_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    u64 nblk = port->param.nBlock ? port->param.nBlock : 1;
    if (blockNo >= nblk) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PARAM;
    }
    u64 t = port->read_index + blockNo - (port->read_index % nblk);
    if (blockNo < port->read_index % nblk) t += nblk;
    mutex_unlock(&s_audio_mutex);

    vm_write64(tag_ea, t);
    return CELL_OK;
}

s32 cellAudioGetPortTimestamp(u32 portNum, u64 tag, u64* stamp)
{
    uint32_t stamp_ea = (uint32_t)(uintptr_t)stamp;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!stamp_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    u64 global_counter = s_ports[portNum].read_index;
    mutex_unlock(&s_audio_mutex);
    if (tag > global_counter)
        return CELL_AUDIO_ERROR_TAG_NOT_FOUND;

    u64 t = s_audio_start_us + tag * 256000000ULL / 48000ULL;
    vm_write64(stamp_ea, t);
    return CELL_OK;
}

/* cellAudioCreateNotifyEventQueue(sys_event_queue_t* id, u64* key)
 *
 * Creates the queue the game will block on for audio-period events, and
 * registers it for notification in one step -- the game never calls
 * SetNotifyEventQueue for this one.
 *
 * This was missing entirely. YDKJ imports it, got the unimplemented-NID path,
 * and then blocked on an id nothing had created: sys_event_queue_receive
 * answered CELL_ESRCH every time and the title logged 1.7 MILLION
 * "ERROR - LIBAUDIO DROPOUT" lines in a 60 s run.
 *
 * Both params are GUEST addresses and both are big-endian out-values; the id
 * is 32-bit and the key 64-bit, so they cannot be written with a plain store. */
s32 cellAudioCreateNotifyEventQueue(u32* id, u64* key)
{
    uint32_t id_ea  = (uint32_t)(uintptr_t)id;
    uint32_t key_ea = (uint32_t)(uintptr_t)key;

    printf("[cellAudio] CreateNotifyEventQueue(id_ea=0x%08X key_ea=0x%08X)\n",
           id_ea, key_ea);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (!id_ea || !key_ea)
        return CELL_AUDIO_ERROR_PARAM;

    /* A distinct key per queue: the notify path resolves queues BY KEY, and the
     * game creates its other queues with key 0, so reusing 0 here would make
     * find_queue_by_key ambiguous and post audio events to whichever matched. */
    static uint64_t s_next_key = 0x8000000000000001ull;
    uint64_t k = s_next_key++;

    uint32_t qid = sys_event_queue_create_direct(k, 8);
    if (!qid)
        return CELL_AUDIO_ERROR_EVENT_QUEUE;

    mutex_lock(&s_audio_mutex);
    int slot = -1;
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { mutex_unlock(&s_audio_mutex); return CELL_AUDIO_ERROR_PARAM; }
    s_notify_queues[slot].in_use = 1;
    s_notify_queues[slot].key    = k;
    mutex_unlock(&s_audio_mutex);

    vm_write32(id_ea, qid);
    vm_write64(key_ea, k);
    printf("[cellAudio] CreateNotifyEventQueue -> id=%u key=0x%llX\n",
           qid, (unsigned long long)k);
    return CELL_OK;
}

s32 cellAudioSetNotifyEventQueue(u64 key)
{
    printf("[cellAudio] SetNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) {
            s_notify_queues[i].in_use = 1;
            s_notify_queues[i].key = key;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM; /* no free slots */
}

s32 cellAudioRemoveNotifyEventQueue(u64 key)
{
    printf("[cellAudio] RemoveNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == key) {
            s_notify_queues[i].in_use = 0;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!config)
        return CELL_AUDIO_ERROR_PARAM;

    /* `config` is a GUEST address; write the BE struct field-by-field at its
     * guest offsets (8-byte aligned u64s). */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];

    /* CellAudioPortConfig (cell/audio.h). sys_addr_t is uintptr_t, which on the
     * 32-bit PPU ABI is FOUR bytes -- not eight. Writing the two sys_addr_t
     * fields as u64 shifted every field past it and left the game reading the
     * high (zero) half of readIndexAddr as its pointer:
     *   sys_addr_t readIndexAddr;  u32 @  0
     *   uint32_t   status;         u32 @  4
     *   uint64_t   nChannel;       u64 @  8
     *   uint64_t   nBlock;         u64 @ 16
     *   uint32_t   portSize;       u32 @ 24
     *   sys_addr_t portAddr;       u32 @ 28      -> 32 bytes */
    vm_write32(cfg +  0, (u32)port->read_idx_addr);                          /* readIndexAddr */
    vm_write32(cfg +  4, port->running ? CELL_AUDIO_STATUS_RUN
                                       : CELL_AUDIO_STATUS_READY);           /* status */
    vm_write64(cfg +  8, port->param.nChannel);                             /* nChannel */
    vm_write64(cfg + 16, port->param.nBlock);                               /* nBlock */
    vm_write32(cfg + 24, port->buf_size);                                   /* portSize */
    vm_write32(cfg + 28, (u32)port->port_addr);                             /* portAddr */

    { static int _n = 0; if (_n++ < 24)
        fprintf(stderr, "[cellAudio] GetPortConfig(port=%u) status=%s bufEA=0x%08X ridxEA=0x%08X\n",
                portNum, port->running ? "RUN" : "READY",
                (u32)port->port_addr, (u32)port->read_idx_addr); }

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortGetStatus(u32 portNum, u32* status)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !status)
        return CELL_AUDIO_ERROR_PARAM;

    if (!s_ports[portNum].in_use) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_CLOSE);
    } else if (s_ports[portNum].running) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_RUN);
    } else {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_READY);
    }

    return CELL_OK;
}

s32 cellAudioSetPersonalDevice(s32 iPersonalStream, s32 iDevice)
{
    (void)iPersonalStream;
    (void)iDevice;
    printf("[cellAudio] SetPersonalDevice(stream=%d, device=%d) - stub\n",
           iPersonalStream, iDevice);
    return CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
    (void)iPersonalStream;
    printf("[cellAudio] UnsetPersonalDevice(stream=%d) - stub\n", iPersonalStream);
    return CELL_OK;
}

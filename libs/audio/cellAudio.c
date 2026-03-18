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

#include "cellAudio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

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

/* Mixing thread */
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
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
        SDL_QueueAudio(s_sdl_audio_dev, stereo_samples,
                       num_samples * 2 * sizeof(float));
    }
}

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

    /* Request event-driven mode with a ~20ms buffer */
    REFERENCE_TIME buf_duration = 200000; /* 20ms in 100ns units */
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

    hr = s_wasapi_client->lpVtbl->GetService(
        s_wasapi_client, &IID_IAudioRenderClient, (void**)&s_wasapi_render);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get render client: 0x%08lX\n", hr);
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
        return -1;
    }

    s_wasapi_client->lpVtbl->Start(s_wasapi_client);
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
    s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available)
        num_samples = available;

    if (num_samples == 0) return;

    BYTE* buf = NULL;
    HRESULT hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        memcpy(buf, stereo_samples, num_samples * 2 * sizeof(float));
        s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, num_samples, 0);
    }
}

static u32 audio_backend_queued_samples(void)
{
    if (!s_wasapi_client) return 0;
    UINT32 padding = 0;
    s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    return padding;
}

#endif /* AUDIO_BACKEND_WASAPI */

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Mix one block from all active ports into s_mix_buffer (stereo float) */
static void audio_mix_one_block(void)
{
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
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

        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float left, right;
            if (nch >= 2) {
                left  = src[s * nch + 0] * level;
                right = src[s * nch + 1] * level;
            } else {
                /* Mono: duplicate to both channels */
                left = right = src[s] * level;
            }

            /* If 7.1, mix center and other channels into stereo */
            if (nch == 8) {
                float center = src[s * 8 + 2] * level * 0.707f;
                float lfe    = src[s * 8 + 3] * level * 0.5f;
                float rl     = src[s * 8 + 4] * level * 0.5f;
                float rr     = src[s * 8 + 5] * level * 0.5f;
                float sl     = src[s * 8 + 6] * level * 0.3f;
                float sr     = src[s * 8 + 7] * level * 0.3f;
                left  += center + lfe + rl + sl;
                right += center + lfe + rr + sr;
            }

            s_mix_buffer[s * 2 + 0] += left;
            s_mix_buffer[s * 2 + 1] += right;
        }

        /* Advance read index */
        port->read_index++;
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
    /*
     * In a full emulator, this would send a sys_event to each registered
     * event queue so the game knows it can write the next audio block.
     * Here we just update state; the game-side event queue integration
     * would be in the sys_event module.
     */
    (void)s_notify_queues; /* suppress unused warning */
}

#ifdef _WIN32
static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    while (s_mix_thread_running) {
        /* Mix and submit one block */
        audio_mix_one_block();
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);

        /* Notify event queues */
        audio_notify_event_queues();

        /* Wait approximately one audio period (~5.333ms).
         * Adjust based on how much is queued to avoid buffer overrun. */
        u32 queued = audio_backend_queued_samples();
        if (queued > CELL_AUDIO_BLOCK_SAMPLES * 4) {
            Sleep(5);
        } else {
            Sleep(2);
        }
    }

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
#endif
    return 0;
}

static void audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
#ifdef _WIN32
    if (s_mix_thread) {
        WaitForSingleObject(s_mix_thread, 2000);
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
#else
    pthread_join(s_mix_thread, NULL);
#endif
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioInit(void)
{
    printf("[cellAudio] Init()\n");

    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    memset(s_notify_queues, 0, sizeof(s_notify_queues));
    mutex_init(&s_audio_mutex);

    if (audio_backend_init() < 0) {
        printf("[cellAudio] WARNING: Audio backend init failed, continuing silently\n");
        /* Don't fail -- games should still run without audio */
    }

    if (audio_start_mix_thread() < 0) {
        printf("[cellAudio] WARNING: Could not start mixing thread\n");
    }

    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    audio_stop_mix_thread();
    audio_backend_shutdown();

    /* Free all port buffers */
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (s_ports[i].buffer) {
            free(s_ports[i].buffer);
            s_ports[i].buffer = NULL;
        }
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }

    mutex_destroy(&s_audio_mutex);
    s_audio_initialized = 0;
    return CELL_OK;
}

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           param ? (unsigned long long)param->nChannel : 0ULL,
           param ? (unsigned long long)param->nBlock : 0ULL);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (!param || !portNum)
        return CELL_AUDIO_ERROR_PARAM;

    /* Validate parameters */
    u64 nch = param->nChannel;
    u64 nblk = param->nBlock;
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
    port->param   = *param;
    port->read_index  = 0;
    port->write_index = 0;

    /* Allocate audio buffer */
    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);
    port->buffer = (float*)calloc(buf_samples, sizeof(float));

    if (!port->buffer) {
        port->in_use = 0;
        mutex_unlock(&s_audio_mutex);
        return CELL_ENOMEM;
    }

    /* Set guest-visible addresses (using host pointers as placeholder).
     * In a full emulator these would be allocated from guest VM memory. */
    port->port_addr    = (u64)(uintptr_t)port->buffer;
    port->read_idx_addr = (u64)(uintptr_t)&port->read_index;

    *portNum = (u32)found;

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

    if (s_ports[portNum].buffer) {
        free(s_ports[portNum].buffer);
        s_ports[portNum].buffer = NULL;
    }

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

    mutex_lock(&s_audio_mutex);

    AudioPortSlot* port = &s_ports[portNum];

    memset(config, 0, sizeof(CellAudioPortConfig));
    config->nChannel      = port->param.nChannel;
    config->nBlock        = port->param.nBlock;
    config->portSize      = port->buf_size;
    config->portAddr      = port->port_addr;
    config->readIndexAddr = port->read_idx_addr;

    if (port->running)
        config->status = CELL_AUDIO_STATUS_RUN;
    else
        config->status = CELL_AUDIO_STATUS_READY;

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
        *status = CELL_AUDIO_STATUS_CLOSE;
    } else if (s_ports[portNum].running) {
        *status = CELL_AUDIO_STATUS_RUN;
    } else {
        *status = CELL_AUDIO_STATUS_READY;
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

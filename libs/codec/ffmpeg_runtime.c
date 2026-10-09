/*
 * ps3recomp - FFmpeg, loaded at run time (see ffmpeg_runtime.h)
 */

#include "ffmpeg_runtime.h"

#ifdef PS3RECOMP_FFMPEG_RUNTIME

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HMODULE ff_lib;
static ff_lib ff_open(const char* p) { return LoadLibraryA(p); }
static void*  ff_sym(ff_lib l, const char* n) { return (void*)(uintptr_t)GetProcAddress(l, n); }
static void   ff_close(ff_lib l) { FreeLibrary(l); }
#else
#include <dlfcn.h>
typedef void* ff_lib;
static ff_lib ff_open(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void*  ff_sym(ff_lib l, const char* n) { return dlsym(l, n); }
static void   ff_close(ff_lib l) { dlclose(l); }
#endif

/* The major the headers describe, which is the only one whose structs we can
 * read: libavcodec.so.63 / avcodec-63.dll / libavcodec.63.dylib. */
#define FF_MAJ_C AV_STRINGIFY(LIBAVCODEC_VERSION_MAJOR)
#define FF_MAJ_U AV_STRINGIFY(LIBAVUTIL_VERSION_MAJOR)
#if defined(_WIN32)
#define FF_AVCODEC_NAME "avcodec-" FF_MAJ_C ".dll"
#define FF_AVUTIL_NAME  "avutil-" FF_MAJ_U ".dll"
#elif defined(__APPLE__)
#define FF_AVCODEC_NAME "libavcodec." FF_MAJ_C ".dylib"
#define FF_AVUTIL_NAME  "libavutil." FF_MAJ_U ".dylib"
#else
#define FF_AVCODEC_NAME "libavcodec.so." FF_MAJ_C
#define FF_AVUTIL_NAME  "libavutil.so." FF_MAJ_U
#endif

static PS3FFmpeg s_ff;
static int s_state;   /* 0 not tried, 1 loaded, -1 unavailable */

static const char* env_or(const char* name, const char* dflt)
{
    const char* v = getenv(name);
    return (v && *v) ? v : dflt;
}

static int ff_load(void)
{
    const char* off = getenv("PS3RECOMP_FFMPEG");
    if (off && strcmp(off, "0") == 0) {
        printf("[ffmpeg] disabled by PS3RECOMP_FFMPEG=0\n");
        return -1;
    }
    const char* util_path  = env_or("PS3RECOMP_FFMPEG_AVUTIL", FF_AVUTIL_NAME);
    const char* codec_path = env_or("PS3RECOMP_FFMPEG_AVCODEC", FF_AVCODEC_NAME);
    /* avutil first: avcodec depends on it, and on Windows the loader finds an
     * already-loaded avutil by name. */
    ff_lib util = ff_open(util_path);
    ff_lib codec = util ? ff_open(codec_path) : NULL;
    if (!util || !codec) {
        printf("[ffmpeg] %s not found; decoders stay callback-only\n",
               util ? codec_path : util_path);
        if (util) ff_close(util);
        return -1;
    }

    int missing = 0;
#define FF_RESOLVE(lib, fn) \
    do { *(void**)&s_ff.fn = ff_sym(lib, #fn); \
         if (!s_ff.fn) { printf("[ffmpeg] missing %s\n", #fn); missing = 1; } } while (0)
    FF_RESOLVE(codec, avcodec_version);
    FF_RESOLVE(codec, avcodec_find_decoder);
    FF_RESOLVE(codec, avcodec_alloc_context3);
    FF_RESOLVE(codec, avcodec_open2);
    FF_RESOLVE(codec, avcodec_free_context);
    FF_RESOLVE(codec, avcodec_send_packet);
    FF_RESOLVE(codec, avcodec_receive_frame);
    FF_RESOLVE(codec, avcodec_flush_buffers);
    FF_RESOLVE(codec, av_packet_alloc);
    FF_RESOLVE(codec, av_packet_free);
    FF_RESOLVE(util, avutil_version);
    FF_RESOLVE(util, av_frame_alloc);
    FF_RESOLVE(util, av_frame_free);
    FF_RESOLVE(util, av_frame_unref);
    FF_RESOLVE(util, av_log_set_level);
#undef FF_RESOLVE

    if (!missing && ((s_ff.avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR ||
                     (s_ff.avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR)) {
        printf("[ffmpeg] library is avcodec %u / avutil %u, built for %d / %d; "
               "not used\n", s_ff.avcodec_version() >> 16,
               s_ff.avutil_version() >> 16,
               LIBAVCODEC_VERSION_MAJOR, LIBAVUTIL_VERSION_MAJOR);
        missing = 1;
    }
    if (missing) {
        memset(&s_ff, 0, sizeof(s_ff));
        ff_close(codec);
        ff_close(util);
        return -1;
    }
    s_ff.av_log_set_level(AV_LOG_ERROR);
    unsigned v = s_ff.avcodec_version();
    printf("[ffmpeg] libavcodec %u.%u.%u loaded\n", v >> 16, (v >> 8) & 0xFF, v & 0xFF);
    /* The libraries stay loaded for the life of the process. */
    return 1;
}

/* Load once, even when two codec modules open from different threads. */
#ifdef _WIN32
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK ff_once(PINIT_ONCE o, PVOID p, PVOID* c)
{
    (void)o; (void)p; (void)c;
    s_state = ff_load();
    return TRUE;
}
#else
#include <pthread.h>
static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static void ff_once(void) { s_state = ff_load(); }
#endif

const PS3FFmpeg* ps3_ffmpeg_get(void)
{
#ifdef _WIN32
    InitOnceExecuteOnce(&s_once, ff_once, NULL, NULL);
#else
    pthread_once(&s_once, ff_once);
#endif
    return s_state > 0 ? &s_ff : NULL;
}

#else

/* ISO C forbids an empty translation unit. */
typedef int ps3_ffmpeg_runtime_disabled;

#endif /* PS3RECOMP_FFMPEG_RUNTIME */

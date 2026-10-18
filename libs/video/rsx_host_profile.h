#ifndef RSX_HOST_PROFILE_H
#define RSX_HOST_PROFILE_H
/* Graphics-thread-only host wall-time buckets. No GPU-time claim. Disabled
 * unless RSX_ENGINE_HOST_PROFILE_CSV names an owned output. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
enum { HP_VERTEX, HP_TARGET, HP_PIPELINE, HP_TEXTURE, HP_DRAW, HP_HASH, HP_UPLOAD, HP_VPREP, HP_VDECODE, HP_VREFS, HP_REMAP, HP_BUCKETS };
static FILE* hp_file;
static int hp_initialized;
static uint64_t hp_elapsed[HP_BUCKETS], hp_calls[HP_BUCKETS];
static uint64_t hp_frequency;
static uint64_t hp_tick(void)
{
    if (!hp_initialized) {
        hp_initialized = 1;
        const char* path = getenv("RSX_ENGINE_HOST_PROFILE_CSV");
        if (path && *path) {
#ifdef _WIN32
            LARGE_INTEGER f;
            if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0 || f.QuadPart > 1000000000) return 0;
            hp_frequency = (uint64_t)f.QuadPart;
#endif
            hp_file = fopen(path,"wb");
            if (hp_file) fprintf(hp_file,"frame,vertex_ns,target_ns,pipeline_ns,texture_ns,draw_ns,hash_ns,upload_ns,vprep_ns,vdecode_ns,vrefs_ns,remap_ns,vertex_calls,target_calls,pipeline_calls,texture_calls,draw_calls,hash_calls,upload_calls,vprep_calls,vdecode_calls,vrefs_calls,remap_calls,end_ns\n");
        }
    }
    if (!hp_file) return 0;
#ifdef _WIN32
    LARGE_INTEGER t; if (!QueryPerformanceCounter(&t)) return 0;
    uint64_t q = (uint64_t)t.QuadPart;
    return (q / hp_frequency)*1000000000u + ((q % hp_frequency)*1000000000u)/hp_frequency;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (uint64_t)t.tv_sec*1000000000u + (uint64_t)t.tv_nsec;
#endif
}
static void hp_end(unsigned bucket, uint64_t start)
{
    if (!start) return;
    uint64_t end = hp_tick();
    if (end >= start) hp_elapsed[bucket] += end-start;
    ++hp_calls[bucket];
}
static void hp_frame(unsigned frame)
{
    if (!hp_file) return;
    fprintf(hp_file,"%u",frame);
    for(unsigned i=0;i<HP_BUCKETS;++i) fprintf(hp_file,",%llu",(unsigned long long)hp_elapsed[i]);
    for(unsigned i=0;i<HP_BUCKETS;++i) fprintf(hp_file,",%llu",(unsigned long long)hp_calls[i]);
    fprintf(hp_file,",%llu\n",(unsigned long long)hp_tick());
    for(unsigned i=0;i<HP_BUCKETS;++i) hp_elapsed[i]=hp_calls[i]=0;
    if (frame % 60 == 0) fflush(hp_file);
}
static void hp_close(void)
{
    if (hp_file) fclose(hp_file);
    hp_file=NULL; hp_initialized=0;
    for(unsigned i=0;i<HP_BUCKETS;++i) hp_elapsed[i]=hp_calls[i]=0;
}
#endif

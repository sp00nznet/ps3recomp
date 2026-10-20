#include "texture_pack_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef SRWLOCK tps_lock;
typedef CONDITION_VARIABLE tps_cond;
#  define TPS_LOCK(l)    AcquireSRWLockExclusive(l)
#  define TPS_UNLOCK(l)  ReleaseSRWLockExclusive(l)
#  define TPS_WAIT(c,l)  SleepConditionVariableSRW(c,l,INFINITE,0)
#  define TPS_SIGNAL(c)  WakeConditionVariable(c)
#  define TPS_BROADCAST(c) WakeAllConditionVariable(c)
static tps_lock s_lock = SRWLOCK_INIT;
static tps_cond s_cond = CONDITION_VARIABLE_INIT;
static uint64_t tps_now_ns(void)
{
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
#  include <dirent.h>
#  include <pthread.h>
#  include <time.h>
typedef pthread_mutex_t tps_lock;
typedef pthread_cond_t tps_cond;
#  define TPS_LOCK(l)    pthread_mutex_lock(l)
#  define TPS_UNLOCK(l)  pthread_mutex_unlock(l)
#  define TPS_WAIT(c,l)  pthread_cond_wait(c,l)
#  define TPS_SIGNAL(c)  pthread_cond_signal(c)
#  define TPS_BROADCAST(c) pthread_cond_broadcast(c)
static tps_lock s_lock = PTHREAD_MUTEX_INITIALIZER;
static tps_cond s_cond = PTHREAD_COND_INITIALIZER;
static uint64_t tps_now_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
#endif

#define TPS_SLOTS   16384u          /* power of two: replacements and requests seen */
#define TPS_WORKERS 2u

enum { E_FREE = 0, E_QUEUED, E_LOADING, E_READY, E_FAILED, E_EVICTED };

typedef struct {
    uint8_t hash[32];
    uint32_t format, sw, sh;
    int present;          /* the pack has this file */
    int state;
    uint32_t refs;
    uint64_t bytes, last_use;
    texture_pack pack;
} tps_entry;

static tps_entry s_entry[TPS_SLOTS];
static uint32_t s_queue[TPS_SLOTS];
static uint32_t s_queue_head, s_queue_count;
static char s_root[4096];
static int s_started, s_stop;
static uint64_t s_budget, s_use_clock;
static texture_pack_stream_stats s_stats;
#ifdef _WIN32
static HANDLE s_worker[TPS_WORKERS];
#else
static pthread_t s_worker[TPS_WORKERS];
#endif

static uint32_t tps_slot_hash(const uint8_t hash[32], uint32_t format, uint32_t sw, uint32_t sh)
{
    uint32_t x;
    memcpy(&x, hash, 4);
    x ^= format * 0x9E3779B1u ^ sw * 0x85EBCA77u ^ sh * 0xC2B2AE3Du;
    return x & (TPS_SLOTS - 1u);
}

/* The slot of a key, or the free slot it would take; UINT32_MAX when full. */
static uint32_t tps_find(const uint8_t hash[32], uint32_t format, uint32_t sw, uint32_t sh)
{
    uint32_t s = tps_slot_hash(hash, format, sw, sh);
    for (uint32_t n = 0; n < TPS_SLOTS; n++, s = (s + 1u) & (TPS_SLOTS - 1u)) {
        const tps_entry *e = &s_entry[s];
        if (e->state == E_FREE && !e->present) return s;
        if (e->format == format && e->sw == sw && e->sh == sh && !memcmp(e->hash, hash, 32)) return s;
    }
    return UINT32_MAX;
}

static int tps_hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* <64 hex>-f<1|2|3>-<w>x<h>.frp marks a replacement as present. */
static void tps_note_file(const char *name)
{
    uint8_t hash[32];
    unsigned format, sw, sh;
    char tail[8];
    if (strlen(name) < 64 + 11) return;   /* shortest: -f1-1x1.frp */
    for (unsigned i = 0; i < 32; i++) {
        const int a = tps_hex(name[2 * i]), b = tps_hex(name[2 * i + 1]);
        if (a < 0 || b < 0) return;
        hash[i] = (uint8_t)(a * 16 + b);
    }
    if (sscanf(name + 64, "-f%u-%ux%u%7s", &format, &sw, &sh, tail) != 4 || strcmp(tail, ".frp") ||
        format < 1 || format > 3 || !sw || !sh || sw > 4096 || sh > 4096)
        return;
    const uint32_t s = tps_find(hash, format, sw, sh);
    if (s == UINT32_MAX) return;
    tps_entry *e = &s_entry[s];
    memcpy(e->hash, hash, 32);
    e->format = format; e->sw = sw; e->sh = sh;
    e->present = 1;
}

static void tps_list(void)
{
#ifdef _WIN32
    char pattern[4200];
    WIN32_FIND_DATAA fd;
    snprintf(pattern, sizeof pattern, "%s\\*.frp", s_root);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do tps_note_file(fd.cFileName); while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(s_root);
    if (!d) return;
    for (struct dirent *de; (de = readdir(d)); ) tps_note_file(de->d_name);
    closedir(d);
#endif
}

/* Frees least recently used unreferenced replacements down to the budget. */
static void tps_trim(void)
{
    while (s_stats.cached_bytes > s_budget) {
        uint32_t victim = UINT32_MAX;
        for (uint32_t i = 0; i < TPS_SLOTS; i++) {
            const tps_entry *e = &s_entry[i];
            if (e->state == E_READY && !e->refs &&
                (victim == UINT32_MAX || e->last_use < s_entry[victim].last_use))
                victim = i;
        }
        if (victim == UINT32_MAX) return;
        tps_entry *e = &s_entry[victim];
        texture_pack_free(&e->pack);
        s_stats.cached_bytes -= e->bytes;
        e->bytes = 0;
        e->state = E_EVICTED;
        s_stats.evictions++;
    }
}

#ifdef _WIN32
static DWORD WINAPI tps_worker(LPVOID arg)
#else
static void *tps_worker(void *arg)
#endif
{
    (void)arg;
    TPS_LOCK(&s_lock);
    for (;;) {
        while (!s_stop && !s_queue_count) TPS_WAIT(&s_cond, &s_lock);
        if (s_stop) break;
        const uint32_t s = s_queue[s_queue_head];
        s_queue_head = (s_queue_head + 1u) & (TPS_SLOTS - 1u);
        s_queue_count--;
        tps_entry *e = &s_entry[s];
        e->state = E_LOADING;
        uint8_t hash[32]; memcpy(hash, e->hash, 32);
        const uint32_t format = e->format, sw = e->sw, sh = e->sh;
        TPS_UNLOCK(&s_lock);
        texture_pack pack;
        const uint64_t begin = tps_now_ns();
        const int ok = texture_pack_load_hashed(s_root, hash, format, sw, sh, &pack);
        const uint64_t spent = tps_now_ns() - begin;
        TPS_LOCK(&s_lock);
        s_stats.load_ns_total += spent;
        if (spent > s_stats.load_ns_max) s_stats.load_ns_max = spent;
        if (ok) {
            uint64_t bytes = 0;
            for (uint32_t m = 0; m < pack.levels; m++) bytes += (uint64_t)pack.row_bytes[m] * pack.level_height[m];
            e->pack = pack;
            e->bytes = bytes;
            e->last_use = ++s_use_clock;
            e->state = E_READY;
            s_stats.cached_bytes += bytes;
            s_stats.loads++;
            tps_trim();
        } else {
            e->state = E_FAILED;
            s_stats.failures++;
        }
    }
    TPS_UNLOCK(&s_lock);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* Under the lock. */
static int tps_start(const char *root)
{
    if (s_started) return strcmp(root, s_root) == 0;
    if (strlen(root) >= sizeof s_root) return 0;
    strcpy(s_root, root);
    const char *mb = getenv("FOLKLORE_TEXTURE_PACK_CACHE_MB");
    const long long v = mb ? atoll(mb) : 2048;
    s_budget = (uint64_t)(v >= 64 ? v : 64) << 20;
    tps_list();

    for (uint32_t i = 0; i < TPS_WORKERS; i++) {
#ifdef _WIN32
        s_worker[i] = CreateThread(NULL, 0, tps_worker, NULL, 0, NULL);
        if (s_worker[i]) SetThreadPriority(s_worker[i], THREAD_PRIORITY_BELOW_NORMAL);
#else
        pthread_create(&s_worker[i], NULL, tps_worker, NULL);
#endif
    }
    s_started = 1;
    return 1;
}

static int tps_hand_out(uint32_t s, texture_pack *out)
{
    tps_entry *e = &s_entry[s];
    e->refs++;
    e->last_use = ++s_use_clock;
    *out = e->pack;
    s_stats.hits++;
    return TEXTURE_PACK_READY;
}

int texture_pack_stream_request(const char *root, const void *base, size_t base_len,
                                uint32_t format, uint32_t sw, uint32_t sh,
                                uint32_t *ticket, texture_pack *out)
{
    uint8_t hash[32];
    if (!ticket || !out) return TEXTURE_PACK_NONE;
    *ticket = 0;
    memset(out, 0, sizeof *out);
    if (!root || !*root || !base || format < 1 || format > 3 || !sw || !sh || sw > 4096 || sh > 4096)
        return TEXTURE_PACK_NONE;
    if (base_len != (size_t)((sw + 3u) / 4u) * ((sh + 3u) / 4u) * (format == 1 ? 8u : 16u))
        return TEXTURE_PACK_NONE;
    texture_pack_sha256(base, base_len, hash);
    int result = TEXTURE_PACK_NONE;
    TPS_LOCK(&s_lock);
    s_stats.requests++;
    if (!s_stop && tps_start(root)) {
        const uint32_t s = tps_find(hash, format, sw, sh);
        tps_entry *e = s == UINT32_MAX ? NULL : &s_entry[s];
        if (!e || !e->present) {
            s_stats.absent++;
        } else {
            *ticket = s + 1u;
            if (e->state == E_READY) {
                result = tps_hand_out(s, out);
            } else if (e->state == E_FAILED) {
                *ticket = 0;
            } else {
                if (e->state == E_FREE || e->state == E_EVICTED) {
                    e->state = E_QUEUED;
                    s_queue[(s_queue_head + s_queue_count) & (TPS_SLOTS - 1u)] = s;
                    s_queue_count++;
                    TPS_SIGNAL(&s_cond);
                }
                result = TEXTURE_PACK_PENDING;
            }
        }
    }
    TPS_UNLOCK(&s_lock);
    return result;
}

int texture_pack_stream_poll(uint32_t ticket, texture_pack *out)
{
    if (!out) return TEXTURE_PACK_NONE;
    memset(out, 0, sizeof *out);
    if (!ticket || ticket > TPS_SLOTS) return TEXTURE_PACK_NONE;
    const uint32_t s = ticket - 1u;
    int result;
    TPS_LOCK(&s_lock);
    tps_entry *e = &s_entry[s];
    if (e->state == E_READY) result = tps_hand_out(s, out);
    else if (e->state == E_QUEUED || e->state == E_LOADING) result = TEXTURE_PACK_PENDING;
    else if (e->state == E_EVICTED && e->present && !s_stop) {
        /* Trimmed before the caller came back: load it again. */
        e->state = E_QUEUED;
        s_queue[(s_queue_head + s_queue_count) & (TPS_SLOTS - 1u)] = s;
        s_queue_count++;
        TPS_SIGNAL(&s_cond);
        result = TEXTURE_PACK_PENDING;
    } else result = TEXTURE_PACK_NONE;
    TPS_UNLOCK(&s_lock);
    return result;
}

void texture_pack_stream_release(uint32_t ticket)
{
    if (!ticket || ticket > TPS_SLOTS) return;
    TPS_LOCK(&s_lock);
    tps_entry *e = &s_entry[ticket - 1u];
    if (e->refs) e->refs--;
    tps_trim();
    TPS_UNLOCK(&s_lock);
}

void texture_pack_stream_get_stats(texture_pack_stream_stats *stats)
{
    if (!stats) return;
    TPS_LOCK(&s_lock);
    *stats = s_stats;
    TPS_UNLOCK(&s_lock);
}

void texture_pack_stream_shutdown(void)
{
    TPS_LOCK(&s_lock);
    const int started = s_started;
    s_stop = 1;
    TPS_BROADCAST(&s_cond);
    TPS_UNLOCK(&s_lock);
    if (started) {
        for (uint32_t i = 0; i < TPS_WORKERS; i++) {
#ifdef _WIN32
            if (s_worker[i]) { WaitForSingleObject(s_worker[i], INFINITE); CloseHandle(s_worker[i]); }
#else
            pthread_join(s_worker[i], NULL);
#endif
        }
    }
    TPS_LOCK(&s_lock);
    for (uint32_t i = 0; i < TPS_SLOTS; i++) texture_pack_free(&s_entry[i].pack);
    memset(s_entry, 0, sizeof s_entry);
    s_queue_head = s_queue_count = 0;
    s_started = 0;
    s_stats.cached_bytes = 0;
    s_stop = 0;
    TPS_UNLOCK(&s_lock);
}

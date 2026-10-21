/* Real MFC registry: count actual read-lock acquisitions and exercise cached
 * owner lifetime/address reuse without changing DMA semantics. */
#include "../../platform/win32_compat.h"
#include <stdatomic.h>
#include <sys/mman.h>
#ifdef MFC_CACHE_GUARDED_ALLOCATOR
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
/* Every registry allocation has its own mapping. Removing its slot unmaps it,
 * making any stale engine use fault even when ASan cannot start on the host. */
static pthread_mutex_t guarded_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { void* pointer; size_t size; } guarded_allocations[1024];
static void* guarded_malloc(size_t size)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t length = (size + page - 1) / page * page;
    void* pointer = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (pointer == MAP_FAILED) return NULL;
    pthread_mutex_lock(&guarded_lock);
    for (unsigned i = 0; i < 1024; ++i) if (!guarded_allocations[i].pointer) {
        guarded_allocations[i].pointer = pointer; guarded_allocations[i].size = length;
        pthread_mutex_unlock(&guarded_lock); return pointer;
    }
    pthread_mutex_unlock(&guarded_lock); munmap(pointer, length); abort();
}
static void guarded_free(void* pointer)
{
    pthread_mutex_lock(&guarded_lock);
    for (unsigned i = 0; i < 1024; ++i) if (guarded_allocations[i].pointer == pointer && pointer) {
        size_t length = guarded_allocations[i].size; guarded_allocations[i].pointer = NULL;
        pthread_mutex_unlock(&guarded_lock);
        if (munmap(pointer, length)) abort();
        return;
    }
    pthread_mutex_unlock(&guarded_lock); free(pointer);
}
#define malloc guarded_malloc
#define free guarded_free
#endif
static SRWLOCK* cache_fixture_lock;
static atomic_uint cache_fixture_reads;
static void cache_fixture_shared(SRWLOCK* lock)
{
    if (lock == cache_fixture_lock) atomic_fetch_add(&cache_fixture_reads, 1);
    AcquireSRWLockShared(lock);
}
#define AcquireSRWLockShared cache_fixture_shared
#define main cache_existing_main
#include "test_spu_mfc_slots.c"
#undef main
#undef AcquireSRWLockShared
struct cache_request { spu_context* ctx; mfc_engine* engine; int action, stable; };
static void* cache_worker(void* opaque)
{
    struct cache_request* req = opaque;
    if (req->action == 1) spu_mfc_release(req->ctx);
    else if (req->action == 2)
        spu_mfc_forget_range((uintptr_t)req->ctx, (uintptr_t)req->ctx + sizeof(*req->ctx));
    else {
        req->engine = mfc_for(req->ctx); req->stable = 1;
        for (unsigned i = 0; i < 10000; ++i)
            if (mfc_for(req->ctx) != req->engine) req->stable = 0;
    }
    return NULL;
}
static void cache_join(struct cache_request* request)
{
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, cache_worker, request) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (cache_existing_main()) return 1;
    cache_fixture_lock = &s_mfc_claim_lock;
    const char* flag = getenv("SPU_MFC_LOOKUP_CACHE");
    int enabled = flag && !strcmp(flag, "1");
    spu_context* ctx = calloc(1, sizeof(*ctx)); CHECK(ctx != NULL);
    mfc_engine* original = mfc_for(ctx); original->tag_completed = 0x12345678;
    atomic_store(&cache_fixture_reads, 0);
    for (unsigned i = 0; i < 100000; ++i) CHECK(mfc_for(ctx) == original);
    unsigned reads = atomic_load(&cache_fixture_reads);
    printf("100000 established lookups: %u registry read locks (cache %d)\n", reads, enabled);
    CHECK(reads == (enabled ? 0u : 100000u));
    CHECK(original->tag_completed == 0x12345678);

    /* A new host thread must resolve the same virtual engine, then reuse it;
     * its own TLS cannot borrow an engine from a previous physical SPU. */
    struct cache_request migration = {.ctx = ctx};
    atomic_store(&cache_fixture_reads, 0); cache_join(&migration);
    CHECK(migration.stable && migration.engine == original);
    CHECK(atomic_load(&cache_fixture_reads) == (enabled ? 1u : 10001u));

    /* Another thread releases only after this owner has ceased engine use.
     * The same context address, without memset, begins with fresh MFC state. */
    struct cache_request release = {.ctx = ctx, .action = 1}; cache_join(&release);
    mfc_engine* fresh = mfc_for(ctx);
    CHECK(fresh->queue_count == 0 && fresh->tag_completed == 0xFFFFFFFFu);
    fresh->queue_count = 7; fresh->tag_completed = 0;
    struct cache_request forget = {.ctx = ctx, .action = 2}; cache_join(&forget);
    fresh = mfc_for(ctx);
    CHECK(fresh->queue_count == 0 && fresh->tag_completed == 0xFFFFFFFFu);
    spu_mfc_release(ctx); spu_mfc_release(ctx); free(ctx);

    /* Abandoned context memory is unreadable. Cleanup must compare addresses
     * without trying to clear a cache field inside the dead stack owner. */
    void* inaccessible = mmap(NULL, sizeof(spu_context), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(inaccessible != MAP_FAILED);
    spu_context* abandoned = inaccessible;
    mfc_for(abandoned)->tag_completed = 0;
    struct cache_request dead = {.ctx = abandoned, .action = 2}; cache_join(&dead);
    CHECK(mfc_for(abandoned)->tag_completed == 0xFFFFFFFFu);
    spu_mfc_release(abandoned);
    CHECK(munmap(inaccessible, sizeof(spu_context)) == 0);
    CHECK(s_mfc_slots == NULL);
    /* Saturation must never wrap into a generation that can bless stale TLS.
     * Once saturated, even repeated live-owner hits take the locked path. */
    atomic_store(&s_mfc_cache_generation, UINT64_MAX - 1);
    spu_context* terminal = calloc(1, sizeof(*terminal)); CHECK(terminal != NULL);
    (void)mfc_for(terminal); spu_mfc_release(terminal);
    CHECK(atomic_load(&s_mfc_cache_generation) == UINT64_MAX);
    mfc_engine* terminal_engine = mfc_for(terminal);
    atomic_store(&cache_fixture_reads, 0);
    for (unsigned i = 0; i < 100; ++i) CHECK(mfc_for(terminal) == terminal_engine);
    CHECK(atomic_load(&cache_fixture_reads) == 100);
    terminal_engine->tag_completed = 0; spu_mfc_release(terminal);
    CHECK(atomic_load(&s_mfc_cache_generation) == UINT64_MAX);
    CHECK(mfc_for(terminal)->tag_completed == 0xFFFFFFFFu);
    spu_mfc_release(terminal); free(terminal);
    CHECK(s_mfc_slots == NULL);
    printf("SPU MFC lookup cache: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

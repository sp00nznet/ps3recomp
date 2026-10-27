/* Existing engines are directory reads; only publication/removal need a writer.
 * Real production lookup and real platform locks are used. Every wait is bounded. */
#include "../../platform/win32_compat.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static SRWLOCK* fixture_lock;
static atomic_int fixture_exclusive_attempts;
static atomic_int fixture_gate;
static atomic_int fixture_arrived;
static atomic_int fixture_gate_timeout;
static void fixture_exclusive(SRWLOCK* lock)
{
    if (lock == fixture_lock) {
        atomic_fetch_add(&fixture_exclusive_attempts, 1);
        if (atomic_load(&fixture_gate)) {
            atomic_fetch_add(&fixture_arrived, 1);
            ULONGLONG begin = GetTickCount64();
            while (atomic_load(&fixture_arrived) < 16) {
                if (GetTickCount64() - begin > 3000) {
                    atomic_store(&fixture_gate_timeout, 1);
                    break;
                }
                YieldProcessor();
            }
        }
    }
    AcquireSRWLockExclusive(lock);
}
#define AcquireSRWLockExclusive fixture_exclusive
#define main registry_previous_main
#include "test_spu_mfc_slots.c"
#undef main
#undef AcquireSRWLockExclusive

struct lookup_request {
    spu_context* ctx;
    mfc_engine* engine;
    atomic_int started, done;
    int operation;
};
static void* lookup_worker(void* arg)
{
    struct lookup_request* req = arg;
    atomic_store(&req->started, 1);
    if (req->operation == 1) spu_mfc_release(req->ctx);
    else if (req->operation == 2)
        spu_mfc_forget_range((uintptr_t)req->ctx, (uintptr_t)(req->ctx + 1));
    else req->engine = mfc_for(req->ctx);
    atomic_store(&req->done, 1);
    return NULL;
}
static int wait_flag(atomic_int* flag, unsigned milliseconds)
{
    ULONGLONG begin = GetTickCount64();
    while (!atomic_load(flag)) {
        if (GetTickCount64() - begin > milliseconds) return 0;
        YieldProcessor();
    }
    return 1;
}
static void read_lock_case(spu_context* ctx, int operation)
{
    struct lookup_request req = {.ctx = ctx, .operation = operation};
    pthread_t worker;
    atomic_store(&fixture_exclusive_attempts, 0);
    AcquireSRWLockShared(&s_mfc_claim_lock);
    CHECK(pthread_create(&worker, NULL, lookup_worker, &req) == 0);
    CHECK(wait_flag(&req.started, 3000));
    if (operation == 0) {
        /* On the parent the worker needs a writer, so this reports RED before
         * the root releases its read lock and joins the worker normally. */
        CHECK(wait_flag(&req.done, 1000));
        CHECK(atomic_load(&fixture_exclusive_attempts) == 0);
    } else {
        CHECK(wait_flag(&fixture_exclusive_attempts, 3000));
        CHECK(!wait_flag(&req.done, 50));
    }
    ReleaseSRWLockShared(&s_mfc_claim_lock);
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(atomic_load(&req.done));
    if (!operation) CHECK(req.engine != NULL);
}
int main(void)
{
    /* Retain the pre-existing ownership, 48 live allocation lifetimes, real
     * stalled-list DMA, same-address reset and range cleanup regressions. */
    if (registry_previous_main()) return 1;
    CHECK(s_mfc_slots == NULL);
    fixture_lock = &s_mfc_claim_lock;
    spu_context* ctx = calloc(1, sizeof(*ctx));
    CHECK(ctx != NULL);
    (void)mfc_for(ctx);
    read_lock_case(ctx, 0);
    read_lock_case(ctx, 1);
    CHECK(s_mfc_slots == NULL);
    (void)mfc_for(ctx);
    read_lock_case(ctx, 2);
    CHECK(s_mfc_slots == NULL);
    free(ctx);

    /* All misses reach exclusive admission before any can publish. The
     * exclusive path must recheck or duplicate nodes appear for each owner. */
    struct lookup_request req[16] = {0};
    pthread_t threads[16];
    spu_context* pair[2] = {calloc(1, sizeof(*ctx)), calloc(1, sizeof(*ctx))};
    CHECK(pair[0] && pair[1]);
    atomic_store(&fixture_gate, 1);
    atomic_store(&fixture_arrived, 0);
    for (int i = 0; i < 16; ++i) {
        req[i].ctx = pair[i & 1];
        CHECK(pthread_create(&threads[i], NULL, lookup_worker, &req[i]) == 0);
    }
    for (int i = 0; i < 16; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    atomic_store(&fixture_gate, 0);
    CHECK(!atomic_load(&fixture_gate_timeout));
    for (int i = 0; i < 16; ++i) CHECK(req[i].engine == req[i & 1].engine);
    CHECK(req[0].engine && req[1].engine && req[0].engine != req[1].engine);
    int nodes = 0;
    for (spu_mfc_slot* slot = s_mfc_slots; slot; slot = slot->next) ++nodes;
    CHECK(nodes == 2);
    for (int i = 0; i < 2; ++i) { spu_mfc_release(pair[i]); free(pair[i]); }
    CHECK(s_mfc_slots == NULL);
    printf("SPU MFC shared lookup: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* SPU thread-group syscalls (lv2_register.c): teardown and error returns.
 *
 *   - sys_spu_thread_group_destroy frees each thread's context and local store, so it must not run while a thread is
 *     still executing on its host thread: that is a use-after-free in the thread. A running thread makes it wait a
 *     moment and then answer CELL_EBUSY with nothing freed; once the thread has stopped it succeeds.
 *   - sys_spu_thread_group_start on a group whose thread is still running is CELL_ESTAT.
 *   - A group's unused thread slots are invalid (index MAX_SPU_THREADS), not index 0, which is some other group's thread.
 *   - Error paths return the CELL_E* code. lv2_try_syscall stores the handler's return value in r3, so a handler that
 *     set r3 and then `return -1` handed the guest 0xFFFFFFFFFFFFFFFF.
 *   - sys_spu_thread_get_spu_cfg stores the value through its pointer argument; sys_spu_thread_connect_event on an
 *     unknown thread is ESRCH; sys_spu_image_import answers CELL_EFAULT / CELL_ENOEXEC, not host errnos.
 *
 * clang -std=gnu17 -I include -I runtime -I runtime/syscalls -I runtime/spu -I libs/video
 *   runtime/spu/tests/test_spu_group_lifecycle.c -Wl,-dead_strip -pthread -o /tmp/test_spu_group_lifecycle
 */
#include <stdint.h>
/* Syscall 14's classifier (#341) lives in lv2_lock_line.c, which pulls in the
 * VM map table; nothing here issues syscall 14. */
int64_t lv2_spu_lock_line_classify(uint32_t a, uint64_t f) { (void)a; (void)f; return 0; }
#include "../../syscalls/lv2_register.c"
#include <assert.h>
#include <stdio.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
#ifndef _WIN32
void SetLastError(DWORD e) { (void)e; }
#endif
uint32_t spurs_port_queue(uint32_t port) { (void)port; return 0; }
int64_t sys_event_flag_set(ppu_context* c) { (void)c; return 0; }
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail) { (void)image; (void)max_avail; return 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* sys_event_queue_receive, as the receive_event service sees it: time out
 * fake_timeouts times, then deliver one event in r4-r7 and answer CELL_OK.
 * Records the queue and the timeout it was asked for. */
static int fake_timeouts, fake_calls;
static uint32_t fake_queue; static uint64_t fake_timeout;
static spu_context* fake_stop_on_timeout;   /* ask this context to stop at the first timeout */
int64_t sys_event_queue_receive(ppu_context* c)
{
    fake_calls++;
    fake_queue = (uint32_t)c->gpr[3];
    fake_timeout = c->gpr[5];
    if (fake_timeouts > 0) {
        fake_timeouts--;
        if (fake_stop_on_timeout) fake_stop_on_timeout->stop_request = 1;
        return (int64_t)(int32_t)CELL_ETIMEDOUT;
    }
    c->gpr[4] = 0x1234; c->gpr[5] = 0xA1; c->gpr[6] = 0xB2; c->gpr[7] = 0xC3;
    return 0;
}

/* A lifted thread on its host thread: runs until its context is asked to
 * stop, then leaves the way spu_exec_thread_proc does. */
#ifdef _WIN32
static DWORD WINAPI fake_worker(LPVOID arg)
#else
#include <pthread.h>
#include <unistd.h>
static void* fake_worker(void* arg)
#endif
{
    spu_thread_t* t = (spu_thread_t*)arg;
    while (!((spu_context*)t->sctx)->stop_request) {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(200);
#endif
    }
    __atomic_store_n(&t->running, 0, __ATOMIC_RELEASE);
    return 0;
}

static uint32_t rd32(uint32_t a) { const uint8_t* p = vm_base + a; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Run a handler as lv2_try_syscall does: r3 receives the return value. */
static uint32_t sc(int64_t (*h)(ppu_context*), uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d;
    ctx.gpr[3] = (uint64_t)h(&ctx);
    return (uint32_t)ctx.gpr[3];
}

/* A group with one thread that owns a lifted context and a local store. */
static spu_thread_t* make_thread(spu_group_t** gout)
{
    spu_group_t* g = spu_alloc_group(); assert(g);
    spu_thread_t* t = spu_alloc_thread(); assert(t);
    t->group_id = g->id;
    g->num_threads = 1;
    g->thread_indices[0] = (uint32_t)(t - s_spu_threads);
    t->sctx = (struct spu_context*)calloc(1, sizeof(spu_context)); assert(t->sctx);
    t->local_store = (uint8_t*)calloc(1, 64); assert(t->local_store);
    if (gout) *gout = g;
    return t;
}

int main(void)
{
    vm_base = calloc(1, 1u << 20); assert(vm_base);
    ppu_vm_size = 1u << 20;

    /* Unused thread slots of a fresh group are invalid, not index 0. */
    {
        spu_group_t* g = spu_alloc_group(); assert(g);
        int ok = 1;
        for (int k = 0; k < 8; k++) ok &= g->thread_indices[k] == MAX_SPU_THREADS;
        CHECK(ok, "a fresh group's thread slots are all invalid");
        g->in_use = 0;
    }

    /* Error returns are the codes, not -1. */
    CHECK(sc(sys_spu_thread_group_start_handler, 0xDEAD, 0, 0, 0) == (uint32_t)CELL_ESRCH,
          "group_start of an unknown group is ESRCH");
    CHECK(sc(sys_spu_thread_get_exit_status_handler, 0xDEAD, 0x100, 0, 0) == (uint32_t)CELL_ESRCH,
          "get_exit_status of an unknown thread is ESRCH");
    CHECK(sc(sys_spu_thread_connect_event_handler, 0xDEAD, 1, 1, 0) == (uint32_t)CELL_ESRCH,
          "connect_event on an unknown thread is ESRCH");
    CHECK(sc(sys_spu_thread_write_ls_handler, 0xDEAD, 0, 0, 4) == (uint32_t)CELL_ESRCH,
          "write_ls on an unknown thread is ESRCH");
    CHECK(sc(sys_spu_thread_get_spu_cfg_handler, 0xDEAD, 0x100, 0, 0) == (uint32_t)CELL_ESRCH,
          "get_spu_cfg on an unknown thread is ESRCH");
    CHECK(sc(sys_spu_image_import_handler, 0x2000, 0, 0, 0) == (uint32_t)CELL_EFAULT,
          "image_import with a null source is EFAULT");
    memset(vm_base + 0x3000, 0, 16);
    CHECK(sc(sys_spu_image_import_handler, 0x2000, 0x3000, 0, 0) == (uint32_t)CELL_ENOEXEC,
          "image_import of something that is not an ELF is ENOEXEC");

    /* A thread that is still running. */
    spu_group_t* g = NULL;
    spu_thread_t* t = make_thread(&g);
    t->running = 1;
    CHECK(sc(sys_spu_thread_get_exit_status_handler, t->tid, 0x100, 0, 0) == (uint32_t)CELL_ESTAT,
          "get_exit_status of a running thread is ESTAT (got 0x%08X)",
          sc(sys_spu_thread_get_exit_status_handler, t->tid, 0x100, 0, 0));
    CHECK(sc(sys_spu_thread_group_start_handler, g->id, 0, 0, 0) == (uint32_t)CELL_ESTAT,
          "group_start with a running thread is ESTAT");

    /* get_spu_cfg stores through the pointer. */
    t->spu_cfg = 3;
    memset(vm_base + 0x400, 0xEE, 16);
    CHECK(sc(sys_spu_thread_get_spu_cfg_handler, t->tid, 0x400, 0, 0) == 0, "get_spu_cfg succeeds");
    CHECK(rd32(0x400) == 0 && rd32(0x404) == 3, "get_spu_cfg stores 3 through the pointer (0x%08X 0x%08X)",
          rd32(0x400), rd32(0x404));
    CHECK(vm_base[0x408] == 0xEE, "and nothing past the u64");

    /* group_destroy with the thread running: refused, nothing freed. */
    uint8_t* ls = t->local_store; struct spu_context* sctx = t->sctx;
    CHECK(sc(sys_spu_thread_group_destroy_handler, g->id, 0, 0, 0) == (uint32_t)CELL_EBUSY,
          "group_destroy with a running thread is EBUSY");
    CHECK(t->in_use && g->in_use && t->local_store == ls && t->sctx == sctx,
          "and the thread, its context and its local store are untouched");

    /* The thread stops: the same destroy now goes through. */
    t->running = 0;
    CHECK(sc(sys_spu_thread_group_destroy_handler, g->id, 0, 0, 0) == 0, "group_destroy of a stopped group succeeds");
    CHECK(!t->in_use && !g->in_use && !t->local_store && !t->sctx, "and releases the thread and the group");

    /* group_terminate stops a thread that is still running, as lv2 does: it asks
     * the thread's context to stop and waits for the host thread to leave, so the
     * destroy that follows goes through instead of answering EBUSY. Only marking
     * the group stopped left FMOD's mixer running, its destroy refused and its
     * restart rebuilt on a half torn-down state. */
    {
        spu_group_t* g2 = NULL;
        spu_thread_t* t2 = make_thread(&g2);
        t2->running = 1;
#ifdef _WIN32
        HANDLE th = CreateThread(NULL, 0, fake_worker, t2, 0, NULL);
        CHECK(th != NULL, "worker thread starts");
#else
        pthread_t th;
        CHECK(pthread_create(&th, NULL, fake_worker, t2) == 0, "worker thread starts");
#endif
        CHECK(sc(sys_spu_thread_group_terminate_handler, g2->id, 7, 0, 0) == 0, "group_terminate succeeds");
        CHECK(((spu_context*)t2->sctx)->stop_request == 1, "and asks the running thread's context to stop");
        CHECK(!__atomic_load_n(&t2->running, __ATOMIC_ACQUIRE), "and returns once the thread has left");
        CHECK(g2->cause == SPU_GROUP_CAUSE_TERMINATED && g2->exit_status == 7,
              "the group reports TERMINATED with the status it was given");
        ((spu_context*)t2->sctx)->stop_request = 1;   /* a handler that did not ask must not hang the test */
#ifdef _WIN32
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
#else
        pthread_join(th, NULL);
#endif
        CHECK(sc(sys_spu_thread_group_destroy_handler, g2->id, 0, 0, 0) == 0,
              "group_destroy after group_terminate succeeds");
    }

    /* sys_spu_thread_bind_queue / unbind_queue: the queue a thread's
     * sys_spu_thread_receive_event (stop 0x110) waits on, by spuq number. */
    CHECK(sc(sys_spu_thread_bind_queue_handler, 0x2100, 5, 0x63, 0) == 0, "bind_queue succeeds");
    CHECK(spu_thread_bound_queue(0x2100, 0x63) == 5, "the spuq number maps to the bound queue");
    CHECK(spu_thread_bound_queue(0x2100, 0x64) == 0 && spu_thread_bound_queue(0x2101, 0x63) == 0,
          "another number or another thread maps to nothing");
    CHECK(sc(sys_spu_thread_bind_queue_handler, 0x2100, 6, 0x63, 0) == 0 &&
          spu_thread_bound_queue(0x2100, 0x63) == 6, "binding the number again replaces the queue");

    /* The receive_event service: wait on the bound queue and hand back
     * {status, data1, data2, data3}. */
    {
        spu_context* sc2 = (spu_context*)calloc(1, sizeof(spu_context)); assert(sc2);
        sc2->spu_id = 0x2100;
        uint32_t reply[4] = {0, 0, 0, 0};
        fake_calls = 0; fake_timeouts = 2; fake_stop_on_timeout = NULL;
        CHECK(spu_receive_event_wait(sc2, 0x63, reply) == 1, "receive_event on a bound queue is served");
        CHECK(fake_queue == 6 && fake_calls == 3, "it waits on the bound queue through its timeouts (queue %u, %d calls)",
              fake_queue, fake_calls);
        CHECK(fake_timeout > 0, "in bounded slices, not forever");
        CHECK(reply[0] == 0 && reply[1] == 0xA1 && reply[2] == 0xB2 && reply[3] == 0xC3,
              "the reply is {status, data1, data2, data3} (%08X %08X %08X %08X)", reply[0], reply[1], reply[2], reply[3]);

        fake_calls = 0;
        CHECK(spu_receive_event_wait(sc2, 0x64, reply) == 0 && fake_calls == 0,
              "a number with no queue is left unserviced");

        /* Asked to stop while it waits (a group terminate): it gives up. */
        fake_timeouts = 1000; fake_stop_on_timeout = sc2;
        CHECK(spu_receive_event_wait(sc2, 0x63, reply) == -1, "a thread asked to stop leaves the wait");
        fake_timeouts = 0; fake_stop_on_timeout = NULL;

        CHECK(sc(sys_spu_thread_unbind_queue_handler, 0x2100, 0x63, 0, 0) == 0 &&
              spu_thread_bound_queue(0x2100, 0x63) == 0, "unbind_queue removes the binding");
        CHECK(spu_receive_event_wait(sc2, 0x63, reply) == 0, "and the number is unserviced again");
        free(sc2);
    }

    printf("[spu_group_lifecycle] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}

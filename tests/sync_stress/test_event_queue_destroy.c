/* sys_event_queue_destroy and timed receive.
 *
 *   - mode is honoured: an unknown mode is EINVAL; a plain destroy while a
 *     receiver is blocked is EBUSY and leaves that receiver blocked; a FORCE
 *     destroy returns every blocked receiver CELL_ECANCELED. The lock was
 *     deleted under a receiver parked with no timeout, which then never woke.
 *   - ports connected to a destroyed queue are disconnected (send is ENOTCONN),
 *     so a send cannot land in whatever queue next reuses the slot.
 *   - a timed receive runs to an absolute deadline: wake-ups that find the queue
 *     empty do not restart the timeout.
 *
 * Win32 host threads, through the shim off Windows. Links sys_event.c and
 * test_shims.c only. */
#include "runtime/platform/win32_compat.h"
#include "runtime/syscalls/sys_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;

/* sys_event.c's title-diagnostic side paths reach for these; none can run here. */
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail) { (void)image; (void)max_avail; return 0; }
int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size, uint32_t args_ea)
{ (void)image; (void)image_size; (void)args_ea; return 0; }
void ppu_dump_guest_stack(ppu_context* ctx, const char* tag) { (void)ctx; (void)tag; }
uint32_t yz_thread_current_id(void) { return 0; }

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
#define CHECK_RC(call, want) do { int32_t _rc = (int32_t)(call); \
    if (_rc != (int32_t)(want)) { fprintf(stderr, "FAIL line %d: %s -> 0x%08X, want 0x%08X\n", \
        __LINE__, #call, (unsigned)_rc, (unsigned)(want)); failures++; } } while (0)

typedef int64_t (*sc_fn)(ppu_context*);
static int32_t call(sc_fn f, uint32_t tid, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.thread_id = tid;
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d;
    return (int32_t)f(&ctx);
}
static uint32_t be_read(unsigned off)
{
    const uint8_t* p = vm_base + off;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

typedef struct recv {
    uint32_t q;
    uint64_t timeout_us;
    uint32_t event_at;
    volatile int32_t rc;
    double ms;
    HANDLE h;
} recv_t;
static DWORD WINAPI recv_main(void* p)
{
    recv_t* r = (recv_t*)p;
    const double t0 = now_ms();
    r->rc = call(sys_event_queue_receive, 2, r->q, r->event_at, r->timeout_us, 0);
    r->ms = now_ms() - t0;
    return 0;
}
static void recv_start(recv_t* r, uint32_t q, uint64_t timeout_us, uint32_t event_at)
{
    r->q = q; r->timeout_us = timeout_us; r->event_at = event_at; r->rc = -1; r->ms = 0;
    r->h = CreateThread(NULL, 0, recv_main, r, 0, NULL);
}
static void recv_join(recv_t* r) { WaitForSingleObject(r->h, 20000); CloseHandle(r->h); }

static uint32_t make_queue(void)
{
    CHECK_RC(call(sys_event_queue_create, 1, 0x200, 0, 0x1234, 8), CELL_OK);
    return be_read(0x200);
}
/* Wait until `n` receivers are inside the queue. */
static void wait_waiters(uint32_t q, int n)
{
    for (int i = 0; i < 3000 && g_sys_event_queues[q - 1].waiters != n; i++) Sleep(1);
    CHECK(g_sys_event_queues[q - 1].waiters == n);
}

static void test_destroy_modes(void)
{
    uint32_t q = make_queue();
    CHECK_RC(call(sys_event_queue_destroy, 1, q, 7, 0, 0), CELL_EINVAL);
    CHECK(g_sys_event_queues[q - 1].active);

    /* A port wired to the queue. */
    CHECK_RC(call(sys_event_port_create, 1, 0x210, SYS_EVENT_PORT_LOCAL, 0, 0), CELL_OK);
    const uint32_t port = be_read(0x210);
    CHECK_RC(call(sys_event_port_connect_local, 1, port, q, 0, 0), CELL_OK);
    CHECK_RC(call(sys_event_port_send, 1, port, 1, 2, 3), CELL_OK);          /* queued */

    /* Two receivers: the first takes the queued event, then both block. */
    recv_t r1, r2;
    recv_start(&r1, q, 0, 0x300);
    recv_join(&r1);
    CHECK(r1.rc == CELL_OK);
    recv_start(&r1, q, 0, 0x300);
    recv_start(&r2, q, 0, 0x340);
    wait_waiters(q, 2);

    /* A plain destroy refuses and leaves them blocked. */
    CHECK_RC(call(sys_event_queue_destroy, 1, q, 0, 0, 0), CELL_EBUSY);
    CHECK(g_sys_event_queues[q - 1].active);
    Sleep(50);
    CHECK(r1.rc == -1 && r2.rc == -1);

    /* FORCE wakes both with ECANCELED. */
    CHECK_RC(call(sys_event_queue_destroy, 1, q, SYS_EVENT_QUEUE_DESTROY_FORCE, 0, 0), CELL_OK);
    recv_join(&r1); recv_join(&r2);
    CHECK(r1.rc == (int32_t)CELL_ECANCELED);
    CHECK(r2.rc == (int32_t)CELL_ECANCELED);
    CHECK(!g_sys_event_queues[q - 1].active);

    /* The port no longer points at the dead queue, and a new queue in that slot
     * does not receive from it. */
    CHECK_RC(call(sys_event_port_send, 1, port, 1, 2, 3), CELL_ENOTCONN);
    const uint32_t q2 = make_queue();
    CHECK(q2 == q);                                  /* the slot is reused */
    CHECK_RC(call(sys_event_port_send, 1, port, 1, 2, 3), CELL_ENOTCONN);
    CHECK(g_sys_event_queues[q2 - 1].count == 0);
    CHECK_RC(call(sys_event_queue_receive, 1, q2, 0x300, 1000, 0), CELL_ETIMEDOUT);

    /* Idle: a plain destroy works, and a second one is ESRCH. */
    CHECK_RC(call(sys_event_queue_destroy, 1, q2, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_event_queue_destroy, 1, q2, 0, 0, 0), CELL_ESRCH);
    CHECK_RC(call(sys_event_port_destroy, 1, port, 0, 0, 0), CELL_OK);
}

/* Spurious wake-ups (a broadcast that finds the queue empty) every 25 ms. */
static volatile int s_stop_wakes;
static uint32_t s_wake_q;
static DWORD WINAPI waker(void* unused)
{
    (void)unused;
    while (!s_stop_wakes) {
        Sleep(25);
        sys_event_queue_info* q = &g_sys_event_queues[s_wake_q - 1];
#ifdef _WIN32
        WakeAllConditionVariable(&q->not_empty);
#else
        pthread_cond_broadcast(&q->not_empty);
#endif
    }
    return 0;
}

static void test_timed_receive(void)
{
    const uint32_t q = make_queue();

    /* Plain timeout: not early, not wildly late. */
    recv_t r;
    recv_start(&r, q, 60000, 0x300);
    recv_join(&r);
    CHECK(r.rc == (int32_t)CELL_ETIMEDOUT);
    CHECK(r.ms >= 55.0 && r.ms < 500.0);

    /* A wake-up that finds nothing must not restart the timeout: with one every
     * 25 ms, a 120 ms receive still ends at about 120 ms (it used to start its
     * full wait over after each wake, and so never ended). */
    s_wake_q = q; s_stop_wakes = 0;
    HANDLE w = CreateThread(NULL, 0, waker, NULL, 0, NULL);
    recv_start(&r, q, 120000, 0x300);
    recv_join(&r);
    s_stop_wakes = 1;
    WaitForSingleObject(w, 5000); CloseHandle(w);
    printf("timed receive 120 ms with a wake every 25 ms: rc 0x%08X after %.1f ms\n", (unsigned)r.rc, r.ms);
    CHECK(r.rc == (int32_t)CELL_ETIMEDOUT);
    CHECK(r.ms >= 110.0 && r.ms < 400.0);

    CHECK_RC(call(sys_event_queue_destroy, 1, q, 0, 0, 0), CELL_OK);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 65536);
    if (!vm_base) return 2;
    test_destroy_modes();
    test_timed_receive();
    printf("event queue destroy / timed receive: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

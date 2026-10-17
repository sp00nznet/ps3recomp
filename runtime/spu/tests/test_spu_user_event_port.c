/* An SPU's user event (send_event through the interrupt mailbox) goes to the queue its port is bound to.
 *
 * The port is bound by the PPU with sys_spu_thread_connect_event (this SPU thread) or
 * sys_spu_thread_group_connect_event_all_threads (the group). The SPU thread starts running at
 * sys_spu_thread_group_start while the PPU is still on its way to those calls, so an event for a port that is not
 * bound yet has to wait for the binding for a while instead of being answered with ENOTCONN at once, which makes
 * the SPU program fail its first request. A binding made with sys_spu_thread_connect_event has to be found as well.
 *
 * clang -std=gnu17 -I include -I runtime -I runtime/syscalls -I runtime/spu -I libs/video
 *   runtime/spu/tests/test_spu_user_event_port.c -Wl,-dead_strip -pthread -o /tmp/test_spu_user_event_port
 *   && /tmp/test_spu_user_event_port
 */
#include "../../syscalls/lv2_register.c"
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
void SetLastError(DWORD e) { (void)e; }                       /* the POSIX shim's condition variable reports a timeout */
uint32_t spurs_port_queue(uint32_t port) { (void)port; return 0; }
int64_t sys_event_flag_set(ppu_context* c) { (void)c; return 0; }

/* What the delivery pushes at the queue, recorded instead of sent. */
static uint32_t pushed_queue, pushed_data;
static int pushes;
int sys_event_queue_push_by_id(uint32_t queue_id, uint64_t source, uint64_t d1, uint64_t d2, uint64_t d3)
{
    (void)source; (void)d1; (void)d2;
    pushed_queue = queue_id; pushed_data = (uint32_t)d3; pushes++;
    return 0;
}

static double now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static spu_group_t*  grp;
static spu_thread_t* thr;
static spu_context   spu;

/* sys_spu_thread_connect_event(tid, queue, et = 1, port) */
static void bind_thread(uint32_t queue, uint32_t port)
{
    ppu_context c = {0};
    c.gpr[3] = thr->tid; c.gpr[4] = queue; c.gpr[5] = 1; c.gpr[6] = port;
    assert(sys_spu_thread_connect_event_handler(&c) == 0);
}

/* sys_spu_thread_group_connect_event_all_threads(group, queue, ports_mask, &port) */
static void bind_group(uint32_t queue, uint32_t port)
{
    ppu_context c = {0};
    c.gpr[3] = grp->id; c.gpr[4] = queue; c.gpr[5] = 1ull << port; c.gpr[6] = 0x100;
    assert(sys_spu_thread_group_connect_event_all_threads_handler(&c) == (int64_t)CELL_OK);
}

static void reset(void)
{
    memset(&grp->user_event_ports, 0, sizeof grp->user_event_ports);
    thr->evt_bind_n = 0;
    pushes = 0; pushed_queue = 0;
}

typedef struct { int ms; int via_group; uint32_t queue, port; } late_bind;
static void* late_binder(void* p)
{
    late_bind* b = (late_bind*)p;
    struct timespec ts = { 0, b->ms * 1000000L };
    nanosleep(&ts, NULL);
    if (b->via_group) bind_group(b->queue, b->port); else bind_thread(b->queue, b->port);
    return NULL;
}

/* One send_event on `port` with data 0xBEEF; returns the SPU's answer, and how long the delivery took. */
static uint32_t send_event(uint32_t port, double* took_ms)
{
    spu_channel_write(&spu.ch_out_mbox, 0xBEEFu);
    const double t0 = now_ms();
    assert(spu_deliver_user_event(&spu, (port << 24) | 0x000001u) == 1);
    *took_ms = now_ms() - t0;
    return spu_channel_read(&spu.ch_in_mbox);
}

int main(void)
{
    vm_base = calloc(1, 1u << 20); assert(vm_base);
    ppu_vm_size = 1u << 20;
    grp = spu_alloc_group(); thr = spu_alloc_thread();
    assert(grp && thr);
    grp->thread_indices[0] = (uint32_t)(thr - s_spu_threads); grp->num_threads = 1;
    memset(&spu, 0, sizeof spu);
    spu.spu_group_id = grp->id; spu.spu_id = thr->tid;
    double ms;
    uint32_t r;

    reset(); bind_thread(0x77, 0x2A);
    r = send_event(0x2A, &ms);
    CHECK(r == CELL_OK && pushes == 1 && pushed_queue == 0x77 && pushed_data == 0xBEEF,
          "a port bound with sys_spu_thread_connect_event: answer 0x%08X, %d pushes, queue 0x%X", r, pushes, pushed_queue);

    reset(); bind_group(0x88, 0x05);
    r = send_event(0x05, &ms);
    CHECK(r == CELL_OK && pushes == 1 && pushed_queue == 0x88,
          "a port bound for the group: answer 0x%08X, %d pushes, queue 0x%X", r, pushes, pushed_queue);

    for (int via_group = 0; via_group < 2; via_group++) {
        reset();
        late_bind b = { 40, via_group, 0x99, 0x2A };
        pthread_t th; pthread_create(&th, NULL, late_binder, &b);
        r = send_event(0x2A, &ms);
        pthread_join(th, NULL);
        CHECK(r == CELL_OK && pushes == 1 && pushed_queue == 0x99,
              "a port bound %s 40 ms after the event: answer 0x%08X, %d pushes, queue 0x%X",
              via_group ? "for the group" : "for the thread", r, pushes, pushed_queue);
        CHECK(ms >= 25.0 && ms < 150.0, "the event waited %.0f ms for a binding 40 ms away", ms);
    }

    reset();
    r = send_event(0x11, &ms);
    CHECK(r == CELL_ENOTCONN && pushes == 0, "a port nobody binds: answer 0x%08X, %d pushes", r, pushes);
    CHECK(ms >= 150.0 && ms < 1000.0, "the wait for a port nobody binds took %.0f ms (bounded, about 200)", ms);

    reset();
    spu.spu_id = 0x7FFFFF;                       /* no such thread: nothing will ever bind its port */
    r = send_event(0x2A, &ms);
    CHECK(r == CELL_ENOTCONN && ms < 50.0, "an SPU with no thread: answer 0x%08X after %.0f ms (no wait)", r, ms);

    printf("[spu_user_event_port] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}

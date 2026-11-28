/* Real host threads exercise priority selection, publication, block notification,
 * and detached-thread cleanup. A handshake controls worker progress and a test
 * clock separates those contracts from host scheduling latency. The unmodified
 * wait loop's transitions and exact timeout are tested in test_ppu_prio_wait.c. */
#include "../runtime/platform/win32_compat.h"
static ULONGLONG test_tick(void);
static void test_sleep(DWORD ms);
static BOOL test_yield(void);
#define GetTickCount64 test_tick
#define Sleep test_sleep
#define SwitchToThread test_yield
#ifndef PPU_THREAD_IMPLEMENTATION
#define PPU_THREAD_IMPLEMENTATION "../runtime/syscalls/sys_ppu_thread.c"
#endif
#include PPU_THREAD_IMPLEMENTATION
#undef GetTickCount64
#undef Sleep
#undef SwitchToThread
#include <assert.h>
#include <pthread.h>
#include <time.h>

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x10000;
void ppu_resv_register(ppu_context* c) { (void)c; }
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }

enum { BODY_WRITE_THEN_BLOCK = 1, BODY_WRITE_THEN_RETURN, BODY_SPIN_UNTIL_RELEASED };
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int gate_open, worker_ready, released;
static int g_published, g_fails;
static unsigned clock_calls;
static ULONGLONG virtual_ms;
static uint64_t current_body;
static long host_delay_ms;
#define EXPECT(c) do { if (!(c)) { ++g_fails; fprintf(stderr, "FAIL integration line %d: %s\n", __LINE__, #c); } } while (0)
static long long now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000; }
static void host_sleep(long ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
static unsigned created_tid(void)
{
    uint64_t id; memcpy(&id, vm_base + 0x100, sizeof(id));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    id = __builtin_bswap64(id);
#endif
    assert(id > 0 && id <= PPU_THREAD_MAX);
    return (unsigned)id;
}
static void guest_entry(ppu_context* ctx)
{
    pthread_mutex_lock(&gate_mutex);
    while (!gate_open) pthread_cond_wait(&gate_cond, &gate_mutex);
    pthread_mutex_unlock(&gate_mutex);
    if (host_delay_ms) host_sleep(host_delay_ms);
    if (ctx->gpr[3] != BODY_SPIN_UNTIL_RELEASED)
        __atomic_store_n(&g_published, 1, __ATOMIC_RELAXED);
    if (ctx->gpr[3] == BODY_WRITE_THEN_BLOCK)
        ppu_thread_note_block(ctx->thread_id);
    pthread_mutex_lock(&gate_mutex);
    worker_ready = 1;
    pthread_cond_broadcast(&gate_cond);
    if (ctx->gpr[3] != BODY_WRITE_THEN_RETURN)
        while (!released) pthread_cond_wait(&gate_cond, &gate_mutex);
    pthread_mutex_unlock(&gate_mutex);
    ctx->gpr[3] = 0;
}
static ULONGLONG test_tick(void)
{
    if (clock_calls++ == 0) {
        /* Only entering the runtime wait releases the worker. Non-preempting
         * creates therefore cannot accidentally publish on a busy host. */
        pthread_mutex_lock(&gate_mutex);
        gate_open = 1;
        pthread_cond_broadcast(&gate_cond);
        while (!worker_ready) pthread_cond_wait(&gate_cond, &gate_mutex);
        pthread_mutex_unlock(&gate_mutex);
        if (current_body == BODY_WRITE_THEN_RETURN) {
            /* Freeze test time until the real detached epilogue has completed.
             * The independent loop fixture tests observing a state transition. */
            for (;;) {
                table_lock();
                int done = g_ppu_threads[created_tid() - 1].state == PPU_THREAD_STATE_FREE;
                table_unlock();
                if (done) break;
                host_sleep(1);
            }
        }
    }
    return virtual_ms;
}
static void test_sleep(DWORD ms) { EXPECT(ms == 1); ++virtual_ms; }
static BOOL test_yield(void) { ++virtual_ms; return TRUE; }

static void create(int32_t priority, uint64_t body, int preempts)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.thread_id = 1;
    ctx.gpr[3] = 0x100; ctx.gpr[4] = 0x10000; ctx.gpr[5] = body;
    ctx.gpr[6] = (uint64_t)(int64_t)priority; ctx.gpr[7] = 0x10000;
    current_body = body; clock_calls = 0; virtual_ms = 0;
    const long long start = now_ms();
    EXPECT(sys_ppu_thread_create(&ctx) == 0);
    const long long elapsed = now_ms() - start;
    EXPECT((clock_calls != 0) == preempts);
    EXPECT(virtual_ms == (preempts && body == BODY_SPIN_UNTIL_RELEASED ? PRIO_START_TIMEOUT_MS : 0));
    EXPECT(__atomic_load_n(&g_published, __ATOMIC_RELAXED) == (preempts && body != BODY_SPIN_UNTIL_RELEASED));
    printf("PPU create diagnostic: priority=%d body=%llu preempts=%d host_ms=%lld virtual_ms=%llu\n",
           priority, (unsigned long long)body, preempts, elapsed, (unsigned long long)virtual_ms);
}
static void set_main_priority(int32_t priority)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx)); ctx.gpr[3] = 1; ctx.gpr[4] = (uint64_t)(int64_t)priority;
    EXPECT(sys_ppu_thread_set_priority(&ctx) == 0);
}
static void finish_case(void)
{
    pthread_mutex_lock(&gate_mutex);
    gate_open = released = 1; pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);
    /* The runner's 60s process watchdog diagnoses a hang; no latency threshold
     * is a correctness oracle. Never reset shared state while a worker is live. */
    for (;;) {
        int live = 0;
        table_lock();
        for (int k = 1; k < PPU_THREAD_MAX; ++k)
            if (g_ppu_threads[k].state != PPU_THREAD_STATE_FREE) ++live;
        table_unlock();
        if (!live) break;
        host_sleep(1);
    }
    pthread_mutex_lock(&gate_mutex);
    gate_open = worker_ready = released = 0;
    pthread_mutex_unlock(&gate_mutex);
    __atomic_store_n(&g_published, 0, __ATOMIC_RELAXED);
}
int main(void)
{
    const int on = getenv("PS3_PRIO_START") != NULL;
    const char* delay = getenv("PS3_PRIO_TEST_HOST_DELAY_MS");
    host_delay_ms = delay ? strtol(delay, NULL, 10) : 0;
    assert(host_delay_ms >= 0 && host_delay_ms <= 1000);
    vm_base = calloc(1, 0x10000); assert(vm_base);
    g_vm_stack_alloc.next_addr = 0x10000000u; g_vm_stack_alloc.region_end = 0xA0000000u;
    g_ppu_thread_entry_trampoline = guest_entry;
    ppu_thread_register_main(); set_main_priority(1000);
    create(400, BODY_WRITE_THEN_BLOCK, on); finish_case();
    create(1000, BODY_WRITE_THEN_BLOCK, 0); finish_case();
    create(2000, BODY_WRITE_THEN_BLOCK, 0); finish_case();
    create(400, BODY_WRITE_THEN_RETURN, on); finish_case();
    create(400, BODY_SPIN_UNTIL_RELEASED, on); finish_case();
    set_main_priority(0);
    create(400, BODY_WRITE_THEN_BLOCK, 0); finish_case();
    set_main_priority(300);
    create(400, BODY_WRITE_THEN_BLOCK, 0); finish_case();
    create(200, BODY_WRITE_THEN_BLOCK, on); finish_case();
    free(vm_base);
    if (g_fails) return 1;
    printf("PASS: PS3_PRIO_START %s controlled real-thread integration\n", on ? "on" : "off");
    return 0;
}

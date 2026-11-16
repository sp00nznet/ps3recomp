/* Host producers against guest create/destroy of an event queue.
 *
 * cellAudio's period thread (and SPURS, SPU groups) resolve a queue by key and push
 * into it from their own threads while the guest creates and destroys queues. A
 * producer must never enter a queue lock that is not yet initialised or already
 * destroyed. The guest thread here creates a queue with a key, drains it and
 * destroys it, over and over (both create paths), while producer threads resolve
 * the key and push. Pass: no crash and no lost bookkeeping. On Windows, before the
 * fix, a producer could EnterCriticalSection on a zeroed CRITICAL_SECTION and fault
 * writing near 0x24. */
#include "../sys_event.c"
#include <stdio.h>
#include <stdlib.h>

/* Symbols sys_event.c references that this test never reaches. */
uint8_t* vm_base;
uint32_t g_spu_pending_evt[3];
int g_spu_pending_evt_valid;
void ppu_dump_guest_stack(ppu_context* c, const char* w) { (void)c; (void)w; }
int spu_dispatch_frame_by_queue(uint32_t a, uint32_t b) { (void)a; (void)b; return 0; }
size_t spu_elf_image_size(const uint8_t* i, size_t m) { (void)i; (void)m; return 0; }
int spu_workload_dispatch_async(const uint8_t* i, uint32_t s, uint32_t e) { (void)i; (void)s; (void)e; return 0; }
void ydkj_release_pending_threads(void) {}
void ppu_thread_note_block(uint64_t tid) { (void)tid; }
#ifdef _WIN32
/* sys_timer.c provides these on Windows (inline elsewhere); the timed waits are never reached. */
int64_t lv2_usec_deadline(uint64_t usec) { (void)usec; return 0; }
int lv2_deadline_passed(int64_t deadline) { (void)deadline; return 1; }
#endif

#define KEY 0x8000CAFE00000042ull
#define ROUNDS 4000
#define PRODUCERS 3
static volatile int s_stop;
static volatile long s_pushed, s_rejected;

#ifdef _WIN32
static DWORD WINAPI producer(LPVOID arg)
#else
static void* producer(void* arg)
#endif
{
    (void)arg;
    long ok = 0, no = 0;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        uint32_t qid = sys_event_find_queue_by_key(KEY);
        if (sys_event_queue_push_by_id(qid ? qid : 1, KEY, 1, 2, 3) == 0) ok++; else no++;
    }
    __atomic_fetch_add(&s_pushed, ok, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_rejected, no, __ATOMIC_RELAXED);
    return 0;
}

static void guest_destroy(uint32_t id)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = id;
    if (sys_event_queue_destroy(&ctx) != CELL_OK) { fprintf(stderr, "destroy %u failed\n", id); exit(1); }
}

int main(void)
{
    setvbuf(stderr, NULL, _IOFBF, 1 << 20);   /* the create paths log a line each */
    static uint8_t memory[0x10000];
    vm_base = memory;
#ifdef _WIN32
    HANDLE th[PRODUCERS];
    for (int i = 0; i < PRODUCERS; i++) th[i] = CreateThread(NULL, 0, producer, NULL, 0, NULL);
#else
    pthread_t th[PRODUCERS];
    for (int i = 0; i < PRODUCERS; i++) pthread_create(&th[i], NULL, producer, NULL);
#endif
    for (int r = 0; r < ROUNDS; r++) {
        uint32_t id;
        if (r & 1) {
            id = sys_event_queue_create_direct(KEY, 8);
        } else {
            ppu_context ctx; memset(&ctx, 0, sizeof ctx);
            ctx.gpr[3] = 0x100;   /* id out */
            ctx.gpr[4] = 0;       /* no attributes */
            ctx.gpr[5] = KEY;
            ctx.gpr[6] = 8;
            if (sys_event_queue_create(&ctx) != CELL_OK) { fprintf(stderr, "create failed\n"); return 1; }
            id = ((uint32_t)memory[0x100] << 24) | ((uint32_t)memory[0x101] << 16) |
                 ((uint32_t)memory[0x102] << 8) | memory[0x103];
        }
        if (!id || id > SYS_EVENT_QUEUE_MAX) { fprintf(stderr, "bad id %u\n", id); return 1; }
        guest_destroy(id);
    }
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
#ifdef _WIN32
    WaitForMultipleObjects(PRODUCERS, th, TRUE, INFINITE);
#else
    for (int i = 0; i < PRODUCERS; i++) pthread_join(th[i], NULL);
#endif
    for (int i = 0; i < SYS_EVENT_QUEUE_MAX; i++)
        if (g_sys_event_queues[i].active) { fprintf(stderr, "queue %d left active\n", i + 1); return 1; }
    fflush(stderr);
    printf("test_sys_event_queue_race: ok (%d create/destroy rounds, %ld pushes accepted, %ld rejected)\n",
           ROUNDS, s_pushed, s_rejected);
    return 0;
}

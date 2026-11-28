/* Synthetic SPURS policy-module hop chain through the real scheduler
 * (libs/spurs/cellSpurs.c): cellSpursInitialize, two workloads with
 * hand-written policy modules, the coordinator, the per-SPU workers and
 * spu_run_policy_module. No lifted code, no game data.
 *
 * Each module run is one hop: it records its start, hands the turn to the
 * other workload and raises that workload's ready count, then exits to the
 * kernel. The chain is serial, one module at a time, each started by the
 * previous one's request. Each request also sets a token for the workload it
 * requests; a module run that finds no token (an idle probe by the scheduler)
 * exits without counting.
 *
 *   spu   the module raises the ready count with GETLLAR/PUTLLC on the
 *         instance's first line, as an SPU does.
 *   ppu   the module only records its run; a host thread standing in for the
 *         PPU raises the next ready count with a plain store through the
 *         coherent store path (vm_write8). ppu-idle=N waits N us first, so the
 *         workers are asleep, as they are when a PPU tick releases the next
 *         work; ppu-idle=N~J adds a pseudo-random 0..J us per hop, so the
 *         store's phase against the coordinator's timed pass varies.
 *         The fixture also watches the request line itself and reports how
 *         many of the stand-in's stores reached that watch, and whether the
 *         scheduler's own request-line watch is registered, so the store wake
 *         can be checked without relying on timing.
 *
 * Reported: the latency from one hop's request to the next hop's start (its
 * exit, the worker's bookkeeping, the coordinator's pass and the wake of the
 * worker that runs it), and how often a worker woke with nothing to run.
 *
 * argv: hops (default 2000), lanes (nSpus, 1..6, default 1), mode
 * (spu | ppu | ppu-idle=N[~J]).
 * Fails if the chain stalls. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ps3emu/ps3types.h>
#include "../libs/spurs/cellSpurs.c"
void (*g_rsx_main_release_hook)(uint32_t line);   /* libs/video/rsx_commands.c (#729) */

uint8_t* vm_base;
uint32_t ppu_vm_size = 16u << 20;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
int g_resv_store_active;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void sys_event_queue_cancel_by_id(u32 id) { (void)id; }
void sys_event_queue_uncancel_by_id(u32 id) { (void)id; }
void ydkj_wake_all_event_flags(void) {}
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ (void)ls; (void)a; (void)b; (void)c; (void)d; return 0; }
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
unsigned long long ps3_ms_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return 1000ull + (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
}
u128 spu_rdch(spu_context*, uint32_t);
void spu_wrch(spu_context*, uint32_t, u128);
void spu_indirect_branch(spu_context*);

enum { SPURS_EA = 0x100000, PM_A = 0x200000, PM_B = 0x201000, PM_SIZE = 0x100, WID_OUT = 0x202000 };
static volatile uint32_t g_hops, g_target, g_req[2];   /* g_req: a request is pending for that workload */
static uint32_t g_ppu_kick, g_ppu_idle_us, g_ppu_jitter_us;
/* The stand-in's stores that reached the request line's watches. Watches run
 * on the writing thread, so the flag around the store leaves out the
 * scheduler's own writes. */
static volatile uint32_t g_line_fires;
static _Thread_local int t_ppu_store;
static void count_line_write(void* unused) { (void)unused; if (t_ppu_store) __atomic_add_fetch(&g_line_fires, 1, __ATOMIC_RELAXED); }
static uint32_t g_wid[2];
static uint64_t g_start_ns[1 << 16], g_kick_ns[1 << 16];
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static u128 u32v(uint32_t v) { u128 r; memset(&r, 0, sizeof r); r._u32[0] = v; return r; }
static void ch_w(spu_context* c, uint32_t ch, uint32_t v) { spu_wrch(c, ch, u32v(v)); }
static uint32_t ch_r(spu_context* c, uint32_t ch) { return spu_rdch(c, ch)._u32[0]; }

/* Raise wid's ready count to 1 with an SPU atomic on the instance line. */
static void spu_kick(spu_context* c, uint32_t wid)
{
    for (;;) {
        ch_w(c, MFC_LSA, 0x3F000); ch_w(c, MFC_EAH, 0); ch_w(c, MFC_EAL, SPURS_EA); ch_w(c, MFC_Cmd, MFC_GETLLAR_CMD);
        (void)ch_r(c, MFC_RdAtomicStat);
        c->ls[0x3F000 + wkl_ready_off(wid)] = 1;
        ch_w(c, MFC_LSA, 0x3F000); ch_w(c, MFC_EAH, 0); ch_w(c, MFC_EAL, SPURS_EA); ch_w(c, MFC_Cmd, MFC_PUTLLC_CMD);
        if (ch_r(c, MFC_RdAtomicStat) == 0) return;
    }
}
static void pm_run(spu_context* c, uint32_t me)
{
    if (__atomic_exchange_n(&g_req[me], 0u, __ATOMIC_ACQ_REL)) {
        const uint64_t t = now_ns();
        const uint32_t h = __atomic_load_n(&g_hops, __ATOMIC_ACQUIRE);
        if (h < (1u << 16)) g_start_ns[h] = t;
        if (h + 1 < g_target && !g_ppu_kick) {
            if (h + 1 < (1u << 16)) g_kick_ns[h + 1] = now_ns();
            __atomic_store_n(&g_req[me ^ 1u], 1u, __ATOMIC_RELEASE);
            spu_kick(c, g_wid[me ^ 1u]);
        }
        __atomic_store_n(&g_hops, h + 1, __ATOMIC_RELEASE);
    }
    c->pc = SPURS_PM_EXIT_TO_KERNEL_LS;          /* return to the kernel */
    spu_indirect_branch(c);
}
static void pm_a(spu_context* c) { pm_run(c, 0); }
static void pm_b(spu_context* c) { pm_run(c, 1); }

static int cmp64(const void* a, const void* b) { uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b; return x < y ? -1 : x > y; }

int main(int argc, char** argv)
{
    g_target = argc > 1 ? (uint32_t)atoi(argv[1]) : 2000u;
    const uint32_t lanes = argc > 2 ? (uint32_t)atoi(argv[2]) : 1u;
    const char* mode = argc > 3 ? argv[3] : "spu";
    g_ppu_kick = !strncmp(mode, "ppu", 3);
    if (!strncmp(mode, "ppu-idle=", 9)) {
        char* end = NULL;
        g_ppu_idle_us = (uint32_t)strtoul(mode + 9, &end, 0);
        if (end && *end == '~') g_ppu_jitter_us = (uint32_t)strtoul(end + 1, NULL, 0);
    }
    if (g_target < 2 || g_target > (1u << 16) || lanes < 1 || lanes > 6 ||
        (strcmp(mode, "spu") && !g_ppu_kick)) {
        fprintf(stderr, "usage: %s [hops 2..65536] [lanes 1..6] [spu|ppu|ppu-idle=N[~J]]\n", argv[0]);
        return 2;
    }
    vm_base = calloc(1, ppu_vm_size);
    assert(vm_base);
    for (uint32_t i = 0; i < PM_SIZE; i++) {
        vm_base[PM_A + i] = (uint8_t)(i * 7u + 1u);
        vm_base[PM_B + i] = (uint8_t)(i * 7u + 2u);
    }
    spu_workload_register_img(spu_workload_fingerprint(vm_base + PM_A, PM_SIZE), pm_a, 40, "bench a");
    spu_workload_register_img(spu_workload_fingerprint(vm_base + PM_B, PM_SIZE), pm_b, 41, "bench b");
    assert(cellSpursInitialize((CellSpurs*)(uintptr_t)SPURS_EA, (s32)lanes, 0, 0, 0) == CELL_OK);
    assert(cellSpursAddWorkload((CellSpurs*)(uintptr_t)SPURS_EA, (u32*)(uintptr_t)WID_OUT,
                                (const void*)(uintptr_t)PM_A, PM_SIZE, 0, NULL, 1, 1) == CELL_OK);
    g_wid[0] = vm_read32(WID_OUT);
    assert(cellSpursAddWorkload((CellSpurs*)(uintptr_t)SPURS_EA, (u32*)(uintptr_t)WID_OUT,
                                (const void*)(uintptr_t)PM_B, PM_SIZE, 0, NULL, 1, 1) == CELL_OK);
    g_wid[1] = vm_read32(WID_OUT);

    static spu_coh_watch line_watch;
    if (g_ppu_kick) {
        spu_lockline_lock();
        spu_coh_watch_add(&line_watch, SPURS_EA, count_line_write, NULL);
        spu_lockline_unlock();
    }
    uint32_t seen = 0, ppu_stores = 0, lcg = 0x2545F491u;   /* hop 0 is requested below; the stand-in requests from hop 1 */
    const unsigned long long idle0 = __atomic_load_n(&s_spurs_idle_worker_wakes, __ATOMIC_RELAXED);
    const uint64_t t0 = now_ns();
    g_kick_ns[0] = t0;
    __atomic_store_n(&g_req[0], 1u, __ATOMIC_RELEASE);
    assert(cellSpursReadyCountStore((CellSpurs*)(uintptr_t)SPURS_EA, g_wid[0], 1) == CELL_OK);
    uint32_t last = 0; uint64_t last_t = t0;
    while (__atomic_load_n(&g_hops, __ATOMIC_ACQUIRE) < g_target) {
        const uint32_t h = __atomic_load_n(&g_hops, __ATOMIC_ACQUIRE);
        if (g_ppu_kick && h != seen) {               /* the PPU stand-in: a plain store */
            seen = h;
            if (g_ppu_idle_us) {
                uint32_t us = g_ppu_idle_us;
                if (g_ppu_jitter_us) { lcg = lcg * 1664525u + 1013904223u; us += (lcg >> 8) % (g_ppu_jitter_us + 1); }
                struct timespec d = { (time_t)(us / 1000000u), (long)(us % 1000000u) * 1000 };
                nanosleep(&d, NULL);
            }
            if (h < g_target) {
                if (h < (1u << 16)) g_kick_ns[h] = now_ns();
                __atomic_store_n(&g_req[h & 1u], 1u, __ATOMIC_RELEASE);
                t_ppu_store = 1;
                vm_write8(SPURS_EA + wkl_ready_off(g_wid[h & 1u]), 1);
                t_ppu_store = 0;
                ppu_stores++;
            }
        }
        if (h != last) { last = h; last_t = now_ns(); }
        else if (now_ns() - last_t > 5000000000ull) {
            fprintf(stderr, "FAIL: the chain stalled at hop %u of %u\n", h, g_target);
            return 1;
        }
        if (!g_ppu_kick) { struct timespec d = { 0, 200000 }; nanosleep(&d, NULL); }
    }
    const uint64_t wall = now_ns() - t0;
    const unsigned long long idle = __atomic_load_n(&s_spurs_idle_worker_wakes, __ATOMIC_RELAXED) - idle0;
    const uint32_t n = g_target < (1u << 16) ? g_target : (1u << 16);
    static uint64_t lat[1 << 16];
    uint32_t m = 0;
    for (uint32_t i = 1; i < n; i++) lat[m++] = g_start_ns[i] - g_kick_ns[i];
    qsort(lat, m, sizeof lat[0], cmp64);
    printf("hops=%u lanes=%u mode=%s worker_wake=%d wall_us_per_hop=%.1f p50_us=%.1f p90_us=%.1f idle_worker_wakes=%llu "
           "request_watched=%d ppu_stores=%u line_writes=%u\n",
           g_target, lanes, mode, s_spurs_worker_wake > 0, wall / 1e3 / g_target,
           lat[m / 2] / 1e3, lat[m * 9 / 10] / 1e3, idle, spurs_inst_find(SPURS_EA)->request_watched, ppu_stores,
           __atomic_load_n(&g_line_fires, __ATOMIC_RELAXED));
    if (g_ppu_kick) { spu_lockline_lock(); spu_coh_watch_remove(&line_watch); spu_lockline_unlock(); }
    for (int i = 0; i < 3000; i++) {
        s32 rc = cellSpursFinalize((CellSpurs*)(uintptr_t)SPURS_EA);
        if (rc == CELL_OK) return 0;
        Sleep(1);
    }
    fprintf(stderr, "FAIL: cellSpursFinalize stayed busy\n");
    return 1;
}

/* Queue lifetime is mocked in this scheduler/table fixture. The routing
 * fixture links real LV2 queues and checks generation changes on recreation. */
uint64_t sys_event_queue_generation(uint32_t id) { return id ? 1 : 0; }
void sys_event_queue_cancel_generation(uint32_t id, uint64_t generation)
{ if (generation) sys_event_queue_cancel_by_id(id); }
void sys_event_queue_uncancel_generation(uint32_t id, uint64_t generation)
{ if (generation) sys_event_queue_uncancel_by_id(id); }
uint32_t sys_event_queue_push_generation(uint32_t id, uint64_t generation,
    uint64_t source, uint64_t d1, uint64_t d2, uint64_t d3)
{
    (void)id; (void)generation; (void)source; (void)d1; (void)d2; (void)d3;
    abort(); /* This fixture must not send events. */
}

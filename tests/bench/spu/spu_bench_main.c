/*
 * SPU operation benchmark: operations per second for every SPU operation,
 * lifted and interpreted, against the real SPU runtime.
 *
 * The kernels and their register setup come from gen_spu_bench.py
 * (bench_cases.h); run_spu_bench.sh lifts them, builds this file against
 * runtime/spu, and runs it. Each case is timed by doubling the iteration
 * count until one run takes at least the calibration time (5 ms), then
 * taking the fastest of many runs at that count. Short runs, many of them,
 * and the minimum: a run the OS preempted or put on an efficiency core is
 * slow, never fast, so the minimum is the op's cost on an undisturbed P-core
 * (three 50 ms runs on a loaded machine varied 3x). ops/sec = iterations * ops / s; the
 * loop control (ai + brnz, one pair per 32 ops) is included, not
 * subtracted: the lnop and nop rows (lifted to nothing) are that floor.
 *
 * Usage: spu_bench [--quick] [--only NAME[,NAME...]] [--no-interp] [--repeats N] [--csv FILE]
 */
#include "spu_context.h"
#include "spu_interp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

typedef struct {
    const char* name;
    const char* mode;
    const char* note;
    uint32_t lsa;
    uint32_t ops;
    void (*fn)(spu_context*);
    int nregs;
    struct { uint8_t r; uint32_t w[4]; } regs[48];
} bench_case;

#include "bench_cases.h"

void spu_recomp_register(void);

/* ---- the runtime surface outside the SPU runtime (as test_spu_mfc_slots.c) ---- */
uint8_t* vm_base = NULL;
uint32_t ppu_vm_size = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void) { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0, g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0, g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

/* ---- timing ---- */
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static spu_context* g_ctx;

static void wr_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* Fresh LS and registers for one run of a case. */
static void setup(const bench_case* c, uint32_t iters, int interp)
{
    spu_context* ctx = g_ctx;
    memset(ctx->gpr, 0, sizeof ctx->gpr);
    memcpy(ctx->ls, kLsImage, sizeof kLsImage);
    for (uint32_t k = 0; k < 64; k++)          /* pointer-chase slots point at themselves */
        wr_be32(ctx->ls + BENCH_CHASE_LSA + 16 * k, BENCH_CHASE_LSA + 16 * k);
    for (int i = 0; i < c->nregs; i++)
        for (int j = 0; j < 4; j++)
            ctx->gpr[c->regs[i].r]._u32[j] = c->regs[i].w[j];
    ctx->gpr[2]._u32[0] = iters;
    ctx->gpr[0]._u32[0] = BENCH_STOP_LSA;
    ctx->status = 0;
    ctx->stop_code = 0;
    /* Interpreter runs: image -1 keeps it from handing the kernel to the lifted
     * code (it does that for any registered entry when image_id >= 0). */
    ctx->image_id = interp ? -1 : 0;
    /* Lifted runs are entered as a call, so bi $0 returns to us instead of
     * scheduling a trampoline. */
    ctx->host_depth = interp ? 0 : 1;
}

static double run_once(const bench_case* c, uint32_t iters, int interp)
{
    setup(c, iters, interp);
    double t0 = now_s();
    if (interp)
        spu_interp_run(g_ctx, c->lsa);
    else
        c->fn(g_ctx);
    double t = now_s() - t0;
    g_ctx->tramp_fn = NULL;
    return t;
}

typedef struct { double ops_per_s; double ns_per_op; int ok; } result;

static result measure(const bench_case* c, int interp, double calib_s, int repeats)
{
    result r = {0, 0, 0};
    uint32_t iters = 16;
    double t = run_once(c, iters, interp);
    while (t < calib_s && iters < (1u << 30)) {
        iters *= 2;
        t = run_once(c, iters, interp);
    }
    double best = t;
    for (int i = 1; i < repeats; i++) {
        double ti = run_once(c, iters, interp);
        if (ti < best) best = ti;
    }
    double ops = (double)iters * c->ops;
    r.ops_per_s = ops / best;
    r.ns_per_op = best * 1e9 / ops;
    r.ok = g_ctx->status != SPU_STATUS_STOPPED_BY_HALT;
    return r;
}

static int selected(const char* only, const char* name)
{
    if (!only) return 1;
    size_t n = strlen(name);
    for (const char* p = only; *p;) {
        const char* e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, name, n)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

static void fmt_rate(char* out, size_t sz, double ops)
{
    if (ops >= 1e9) snprintf(out, sz, "%8.2f G", ops / 1e9);
    else if (ops >= 1e6) snprintf(out, sz, "%8.2f M", ops / 1e6);
    else snprintf(out, sz, "%8.2f K", ops / 1e3);
}

int main(int argc, char** argv)
{
    double calib = 0.005;
    int repeats = 25, do_interp = 1;
    const char* only = NULL;
    const char* csv_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--quick")) { calib = 0.002; repeats = 5; }
        else if (!strcmp(argv[i], "--no-interp")) do_interp = 0;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv_path = argv[++i];
        else if (!strcmp(argv[i], "--repeats") && i + 1 < argc) repeats = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s [--quick] [--only a,b] [--no-interp] [--repeats N] [--csv FILE]\n", argv[0]); return 2; }
    }

#if defined(__APPLE__)
    /* Ask for a performance core: the default QoS lets macOS run this on an
     * efficiency core, at a fraction of the speed, whenever it likes. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    /* Guest memory for the DMA and atomic kernels: they transfer to and from
     * BENCH_GUEST_EA (up to 16 KB). */
    ppu_vm_size = BENCH_GUEST_EA + 0x100000u;
    vm_base = (uint8_t*)aligned_alloc(0x10000, ppu_vm_size);
    memset(vm_base, 0, ppu_vm_size);

    g_ctx = (spu_context*)calloc(1, sizeof(spu_context));
    spu_context_init(g_ctx, 0);
    spu_recomp_register();

    FILE* csv = csv_path ? fopen(csv_path, "w") : NULL;
    if (csv) fprintf(csv, "op,mode,note,lifted_ops_per_s,lifted_ns_per_op,interp_ops_per_s,interp_ns_per_op\n");

    printf("%-9s %-13s %-28s %13s %9s %13s %9s %8s\n", "op", "mode", "note",
           "lifted op/s", "ns/op", "interp op/s", "ns/op", "speedup");
    int halted = 0;
    for (int i = 0; i < BENCH_NCASES; i++) {
        const bench_case* c = &kCases[i];
        if (!selected(only, c->name)) continue;
        result L = measure(c, 0, calib, repeats);
        result I = {0, 0, 1};
        if (do_interp) I = measure(c, 1, calib / 2, repeats);
        char lr[32], ir[32];
        fmt_rate(lr, sizeof lr, L.ops_per_s);
        fmt_rate(ir, sizeof ir, I.ops_per_s);
        if (do_interp)
            printf("%-9s %-13s %-28s %13s %9.3f %13s %9.2f %7.0fx%s\n", c->name, c->mode, c->note,
                   lr, L.ns_per_op, ir, I.ns_per_op, L.ops_per_s / I.ops_per_s,
                   (L.ok && I.ok) ? "" : "  HALTED");
        else
            printf("%-9s %-13s %-28s %13s %9.3f%s\n", c->name, c->mode, c->note, lr, L.ns_per_op,
                   L.ok ? "" : "  HALTED");
        halted += !(L.ok && I.ok);
        fflush(stdout);
        if (csv) fprintf(csv, "%s,%s,\"%s\",%.6g,%.6g,%.6g,%.6g\n", c->name, c->mode, c->note,
                         L.ops_per_s, L.ns_per_op, I.ops_per_s, I.ns_per_op);
    }
    if (csv) fclose(csv);
    if (halted) printf("\n%d case(s) halted: their numbers are not valid\n", halted);
    return halted ? 1 : 0;
}

/* A dispatch MISS (no lifted entry registered for the image) through the real
 * runtime/spu/spu_workload.c. The program is dropped, so the runtime must say
 * so loudly: one ERROR line per distinct program across the sync, SPURS job and
 * async paths, and with SPU_MISS_FATAL=1 the process aborts at the first miss.
 * The Python driver (tools/test_spu_dispatch_miss.py) checks stderr and the
 * exit status; this program only drives the dispatches. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spu_workload.h"

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x20000;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
int g_resv_store_active;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void sys_event_queue_cancel_by_id(uint32_t id) { (void)id; }
void sys_event_queue_uncancel_by_id(uint32_t id) { (void)id; }
/* The lifted-job runner references the lv2 publication service even when
 * this test only drives misses. A miss must never create a live context. */
void spu_thread_publish_ctx(uint32_t tid, void* ctx)
{ (void)tid; (void)ctx; abort(); }
/* cellSpurs-side hooks the runtime units reference; a miss never reaches them. */
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
unsigned long long ps3_ms_now(void) { return 0; }
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ (void)ls; (void)a; (void)b; (void)c; (void)d; abort(); }

int main(void)
{
    static uint8_t a[256], b[256];
    memset(a, 0x11, sizeof a);   /* two distinct, never-registered programs */
    memset(b, 0x22, sizeof b);
    int r = 0;
    /* Program a misses on every path, twice each: one ERROR in total. */
    r |= spu_workload_dispatch(a, sizeof a, 0x1000);
    r |= spu_workload_dispatch(a, sizeof a, 0x1000);
    r |= spu_workload_dispatch_job(a, sizeof a, 0x2000, 0x40);
    r |= spu_workload_dispatch_async(a, sizeof a, 0x3000);
    /* Program b is a second dropped program: its own ERROR. */
    r |= spu_workload_dispatch_async(b, sizeof b, 0x4000);
    r |= spu_workload_dispatch_job(b, sizeof b, 0x5000, 0x40);
    fprintf(stderr, "dispatch-miss: done r=%d\n", r);
    return r != 0;   /* a miss must not report the program as run */
}

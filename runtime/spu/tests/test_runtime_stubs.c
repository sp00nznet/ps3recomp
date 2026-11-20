/* Link support for the bring-up tests. The generated spu_recomp.h/spu_dma.h
 * reference runtime hooks (watchpoints, lock guards, restart dispatch, the
 * lockstep gate) that live in spu_channels.c / spu_workload.c, which these
 * tests cannot link because they supply their own spu_rdch/spu_wrch. Every
 * hook here is a weak no-op (a test that links the real unit, e.g. workload via
 * its .deps file, overrides it): none of these tests arms a watchpoint, lockstep gate
 * or overlay note, so reaching one would be a bug in the test. */
#include "spu_recomp.h"
#include <stdint.h>

#define WEAK __attribute__((weak))

WEAK unsigned g_spu_stack_reset_count;
WEAK volatile int g_yz_lockstep_on;
WEAK uint32_t g_ydkj_real_taskset_ea;
WEAK SPU_THREAD_LOCAL volatile unsigned g_wws_batch_gets;
WEAK void (*g_spu_put_hook)(spu_context*, uint32_t, const uint8_t*, uint32_t);

WEAK unsigned long long ps3_ms_now(void) { return 0; }
WEAK void spu_ch_wake(spu_context* ctx) { (void)ctx; }
WEAK void spu_check_stack_reset(spu_context* ctx, void (*fn)(spu_context*)) { (void)ctx; (void)fn; }
WEAK void spu_jmp_no_unwind(void* env) { (void)env; }
WEAK void spu_lockguard_check(spu_context* c, uint32_t ea, const uint8_t* p, uint32_t n, const char* w)
{ (void)c; (void)ea; (void)p; (void)n; (void)w; }
WEAK void spu_overlay_note_get(spu_context* c, uint32_t ea, const uint8_t* p, uint32_t n)
{ (void)c; (void)ea; (void)p; (void)n; }
WEAK unsigned* spu_ls_watch_list(int* out_n) { if (out_n) *out_n = 0; return 0; }
WEAK void spu_mfc_release(spu_context* ctx) { (void)ctx; }
WEAK int spu_run_with_halt(void (*fn)(spu_context*), spu_context* ctx) { fn(ctx); return 0; }
WEAK void spu_restart_dispatch(spu_context* ctx) { (void)ctx; }
WEAK void yz_lockstep_tick(spu_context* ctx) { (void)ctx; }
WEAK void* spu_lookup(uint32_t addr, int image_id) { (void)addr; (void)image_id; return 0; }
WEAK int g_spu_ls_watch_n;
WEAK void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{ (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr; }

/* spu_drain.c owns g_spu_trampoline_fn (the pending cross-function transfer);
 * only the "no stack-reset entry" table is stubbed. */
WEAK uint8_t g_spu_stack_reset_hint[1024];

/* spu_workload.c's taskset/PM paths (not taken by the workload test). */
WEAK uint32_t g_ydkj_real_spurs_ea;
WEAK uint32_t g_ydkj_real_taskid;
WEAK volatile unsigned g_spu_putllc_count;
WEAK volatile unsigned g_spu_putllc_sync_hit;
WEAK void ydkj_wake_all_event_flags(void) {}
WEAK uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t ts, uint32_t tid, uint32_t a, uint32_t b)
{ (void)ls; (void)ts; (void)tid; (void)a; (void)b; return 0; }

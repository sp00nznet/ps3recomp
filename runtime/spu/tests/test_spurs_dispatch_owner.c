/* Real async worker / ELF loader / context owner regression. Unrelated SPU
 * instruction execution and coherency dependencies are stubbed below. */
#include "../spu_workload.c"
#include <assert.h>

/* Async dispatch, the ELF loader and context construction are real. Only
 * unrelated jobs, policy probes, instruction execution and coherency are
 * stubbed. */
uint8_t *vm_base;
uint32_t ppu_vm_size;
uint32_t g_barrier_sync_watch;
void (*g_spu_out_mbox_hook)(uint32_t, uint32_t, int, uint32_t);
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskid, g_ydkj_real_taskset_ea;
unsigned long long ps3_ms_now(void) { return 0; }
#include <stdatomic.h>
static atomic_uint exits;
void spu_coh_notify_write(uint32_t a) {
  if (a == 0x1000 || a == 0x4000)
    atomic_fetch_add(&exits, 1);
}
void spu_coh_unregister(spu_context *c) { (void)c; }
void spu_coh_watch_add(spu_coh_watch *w, uint32_t ea, void (*wake)(void *),
                       void *opaque) {
  (void)w;
  (void)ea;
  (void)wake;
  (void)opaque;
}
void spu_coh_watch_remove(spu_coh_watch *w) { (void)w; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_mfc_release(spu_context *c) { (void)c; }
int spu_run_spurs_job(spu_lifted_entry_fn e, int id, uint32_t ea, uint32_t sz) {
  (void)e;
  (void)id;
  (void)ea;
  (void)sz;
  return -1;
}
int spu_run_with_halt(void (*f)(spu_context *), spu_context *c) {
  f(c);
  return 0;
}
void spu_thread_publish_ctx(uint32_t tid, void *c) {
  (void)tid;
  (void)c;
}
uint64_t spurs_pm_build_context(uint8_t *ls, uint32_t a, uint32_t b, uint32_t c,
                                uint32_t d) {
  (void)ls;
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return 0;
}
int spurs_run_taskset_policy_probe(uint32_t a, uint32_t b, uint32_t c,
                                   uint64_t d, uint32_t e, uint8_t *ls,
                                   uint32_t f) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)ls;
  (void)f;
  return -1;
}
void tsp_spu_func_00000A00(spu_context *c) { (void)c; }
void ydkj_wake_all_event_flags(void) {}

static void prog_a(spu_context *c) { (void)c; }
static void prog_b(spu_context *c) { (void)c; }

int spu_run_spurs_job_owned(spu_lifted_entry_fn e, int id, uint32_t ea,
                            uint32_t sz, uint32_t owner) {
  (void)owner;
  return spu_run_spurs_job(e, id, ea, sz);
}

static atomic_uint entered, gate, observed[2];
static void owner_entry(spu_context *ctx) {
  unsigned index = ctx->spurs_ea == 0x11100 ? 0 : 1;
  assert(ctx->spurs_ea == (index ? 0x22200u : 0x11100u));
  atomic_store(&observed[index], ctx->spurs_ea);
  atomic_fetch_add(&entered, 1);
  while (!atomic_load(&gate))
    Sleep(1);
  assert(ctx->spurs_ea == (index ? 0x22200u : 0x11100u));
}
static void put32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
static void put16(uint8_t *p, uint16_t v) {
  p[0] = v >> 8;
  p[1] = v;
}
static void wait_count(atomic_uint *count, unsigned n) {
  for (unsigned i = 0; i < 5000 && atomic_load(count) < n; i++)
    Sleep(1);
  assert(atomic_load(count) == n);
}
int main(void) {
  vm_base = calloc(1, 0x30000);
  assert(vm_base);
  ppu_vm_size = 0x30000;
  uint8_t *image = vm_base + 0x10000;
  memcpy(image, "\177ELF", 4);
  image[4] = 1;
  image[5] = 2;
  image[6] = 1;
  put16(image + 0x10, 2);
  put16(image + 0x12, 23);
  put32(image + 0x14, 1);
  put32(image + 0x18, 0x3000);
  put32(image + 0x1c, 52);
  put16(image + 0x28, 52);
  put16(image + 0x2a, 32);
  put16(image + 0x2c, 1);
  put32(image + 52, 1);
  put32(image + 56, 84);
  put32(image + 60, 0x3000);
  put32(image + 68, 16);
  put32(image + 72, 16);
  put32(image + 76, 5);
  put32(image + 80, 16);
  put32(vm_base + 0x1064, 0x11100);
  put32(vm_base + 0x4064, 0x22200);
  spu_workload_register_img(spu_workload_fingerprint(image, 100), owner_entry,
                            99, "owner capture fixture");
  g_ydkj_real_taskset_ea = 0x4000;
  g_ydkj_real_taskid = 77;
  g_ydkj_real_spurs_ea = 0xDEAD;
  assert(spu_workload_dispatch_task(image, 100, 0, 0x1000, 0) == 1);
  g_ydkj_real_taskset_ea = 0x1000;
  g_ydkj_real_taskid = 66;
  assert(spu_workload_dispatch_task(image, 100, 0, 0x4000, 0) == 1);
  wait_count(&entered, 2);
  assert(atomic_load(&observed[0]) == 0x11100 &&
         atomic_load(&observed[1]) == 0x22200);
  g_ydkj_real_spurs_ea = 0xBAD;
  atomic_store(&gate, 1);
  wait_count(&exits, 2);
  puts("PASS: two real async workers preserve explicit taskset owner despite "
       "misleading globals");
  /* Detached workers may still retire host contexts after notifying exit.
   * Guest storage remains alive until process exit. */
  return 0;
}

void spu_coh_forget_range(uintptr_t lo, uintptr_t hi) {
  (void)lo;
  (void)hi;
}
void spu_mfc_forget_range(uintptr_t lo, uintptr_t hi) {
  (void)lo;
  (void)hi;
}

/* A raw-SPU program whose loader fills a parameter area in before every load is
 * found whatever that area holds. Resistance's second raw SPU program carries
 * 0x50 bytes of heap pointers, per-level values and its own MMIO window at the
 * end of its image; one run loaded it as 47C9CB480016D493, a Continue from a
 * later checkpoint as 5771C48D28664765, and with a plain fingerprint the second
 * found no lifted entry and the game hung. Registered masked, both resolve to
 * the image's fingerprint with the area zero.
 *
 * tools/test_spu_workload_masked.py builds and runs it:
 *   cc -std=gnu17 -I include -I runtime -I runtime/spu
 *     runtime/spu/tests/test_spu_workload_masked.c -pthread
 *     -o /tmp/test_spu_workload_masked && /tmp/test_spu_workload_masked
 */
#include "../spu_workload.c"
#include <assert.h>

/* The rest of spu_workload.c (dispatch, SPURS jobs, the policy-module runner)
 * is outside this fixture. */
uint8_t* vm_base;
uint32_t g_barrier_sync_watch;
void (*g_spu_out_mbox_hook)(uint32_t, uint32_t, int, uint32_t);
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskid, g_ydkj_real_taskset_ea;
unsigned long long ps3_ms_now(void) { return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_coh_unregister(spu_context* c) { (void)c; }
void spu_lockline_lock(void) { }
void spu_lockline_unlock(void) { }
void spu_mfc_release(spu_context* c) { (void)c; }
int spu_run_spurs_job(spu_lifted_entry_fn e, int id, uint32_t ea, uint32_t sz)
{ (void)e; (void)id; (void)ea; (void)sz; return -1; }
int spu_run_with_halt(void (*f)(spu_context*), spu_context* c) { (void)f; (void)c; return -1; }
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; }
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ (void)ls; (void)a; (void)b; (void)c; (void)d; return 0; }
int spurs_run_taskset_policy_probe(uint32_t a, uint32_t b, uint32_t c, uint64_t d,
                                   uint32_t e, uint8_t* ls, uint32_t f)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)ls; (void)f; return -1; }
void tsp_spu_func_00000A00(spu_context* c) { (void)c; }
void ydkj_wake_all_event_flags(void) { }

static void prog_a(spu_context* c) { (void)c; }
static void prog_b(spu_context* c) { (void)c; }

int main(void)
{
    enum { SIZE = 0x57D0, VAR = 0x5780, VLEN = 0x50 };
    static uint8_t img[SIZE], loaded[SIZE];
    for (unsigned i = 0; i < VAR; i++) img[i] = (uint8_t)(i * 7 + 3);
    const uint64_t fp = spu_workload_fingerprint(img, SIZE);     /* area zero */

    spu_workload_register_img(0x1111ULL, prog_a, 2, "a");
    spu_workload_register_img_masked(fp, prog_b, 3, "b", SIZE, VAR, VLEN);

    /* Two loads with different parameters: neither fingerprints as the image,
     * both resolve to it. */
    for (int run = 0; run < 2; run++) {
        memcpy(loaded, img, SIZE);
        for (unsigned i = 0; i < VLEN; i++) loaded[VAR + i] = (uint8_t)(0x47 + run * 13 + i);
        uint64_t plain = spu_workload_fingerprint(loaded, SIZE), got = 0;
        int id = -1;
        assert(plain != fp);
        assert(spu_workload_find_img(plain, &id) == NULL);
        assert(spu_workload_find_img_masked(loaded, SIZE, &got, &id) == prog_b);
        assert(got == fp && id == 3);
    }

    /* A change outside the area is a different program. */
    memcpy(loaded, img, SIZE);
    loaded[0x100] ^= 1;
    int id = -1;
    assert(spu_workload_find_img_masked(loaded, SIZE, NULL, &id) == NULL && id == 0);
    /* So is the same bytes at another length. */
    assert(spu_workload_find_img_masked(img, SIZE - 0x10, NULL, &id) == NULL);
    /* The plain lookup still finds the image loaded as it is in the ELF. */
    assert(spu_workload_find_img(fp, &id) == prog_b && id == 3);
    /* A masked registration that does not fit its size is refused. */
    spu_workload_register_img_masked(0x2222ULL, prog_a, 4, "bad", 0x100, 0xF0, 0x20);
    assert(spu_workload_find(0x2222ULL) == NULL);

    printf("test_spu_workload_masked: ok\n");
    return 0;
}

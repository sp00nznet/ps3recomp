/* clang -std=gnu17 -I include libs/network/tests/test_score_init.c
 *   -Wl,-dead_strip -o /tmp/test_score_init && /tmp/test_score_init
 */
#include "../sceNp.c"
#include <assert.h>
#include <stdlib.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
/* sceNp.c reports ONLINE and queues its manager callback through these. */
int np_psnr_enabled(void) { return 0; }
const char* np_psnr_online_id(void) { return "PS3Player"; }
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8]) { (void)opd; (void)args; return 0; }
int main(void)
{
    vm_base = calloc(1, 65536);
    assert(vm_base);
    assert((u32)sceNpScoreInit() == SCE_NP_ERROR_NOT_INITIALIZED);
    assert((u32)sceNpScoreTerm() == SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    assert(sceNpInit(131072, (void*)0x1000) == CELL_OK);
    assert(sceNpScoreInit() == CELL_OK);
    assert((u32)sceNpScoreInit() == SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED);
    /* Local score setup must not claim a PSN connection or change its status. */
    assert(sceNpManagerGetStatus((void*)0x100) == CELL_OK);
    assert((s32)vm_read32(0x100) == SCE_NP_MANAGER_STATUS_OFFLINE);
    assert(sceNpScoreTerm() == CELL_OK);
    assert((u32)sceNpScoreTerm() == SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    assert(sceNpScoreInit() == CELL_OK);
    assert(sceNpScoreTerm() == CELL_OK);
    assert(sceNpTerm() == CELL_OK);
    assert((u32)sceNpScoreInit() == SCE_NP_ERROR_NOT_INITIALIZED);
    free(vm_base);
    puts("Score lifecycle and offline status checks passed");
}

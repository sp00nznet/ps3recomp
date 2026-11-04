/*
 * test_np_basic_after_np_init's second file: sceNp.c (sceNpInit/sceNpTerm)
 * and the runtime symbols it links against. See test_np_basic_after_np_init.c.
 */
#include "../sceNp.c"
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
void spu_linelock_lock(uint32_t a) { (void)a; }
void spu_linelock_release(uint32_t a) { (void)a; }
int np_psnr_enabled(void) { return 0; }
const char* np_psnr_online_id(void) { return "PS3Player"; }
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8]) { (void)opd; (void)args; return 0; }
void edat_add_klicensee(const uint8_t klic[16]) { (void)klic; }   /* sceNpDrmIsAvailable (#274) */

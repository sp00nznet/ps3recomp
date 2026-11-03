/*
 * test_edat_guest_klicensee's second file: sceNp.c (sceNpDrmIsAvailable[2])
 * and the runtime symbols it links against. See test_edat_guest_klicensee.c.
 */
#include "../../network/sceNp.c"
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
/* d4: the inline vm_write* family in ppu_memory.h now takes the per-line
 * striped lock, so self-contained fixtures need these stubs too. */
void spu_linelock_lock(uint32_t ea) { (void)ea; }
void spu_linelock_release(uint32_t ea) { (void)ea; }
int np_psnr_enabled(void) { return 0; }
const char* np_psnr_online_id(void) { return "PS3Player"; }
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8]) { (void)opd; (void)args; return 0; }

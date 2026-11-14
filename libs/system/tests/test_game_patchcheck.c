/* cellGamePatchCheck: PARAM for a non-NULL reserved argument (the firmware tests it first), NOTPATCH otherwise,
 * whether or not `size` is given. This runtime never boots a title as a patch, and the size is written only on a
 * patch boot.
 *
 * Built and run by tools/test_game_patchcheck.py. */
#include "../cellGame.c"
#include <assert.h>
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

ps3_guest_caller_fn g_ps3_guest_caller;
const char* ppu_vfs_root;

int main(void)
{
    CellGameContentSize size;
    memset(&size, 0xAB, sizeof size);
    /* The value evidence (static RE, FW 4.93): reserved != NULL -> 0x8002CB07. The non-patch return is the local
     * NOTPATCH macro; the firmware's static non-patch literal (0x8002CB06, its INTERNAL value) was only proven on a
     * successful lookup and the live boot value is unobserved, so the macro mapping is not pinned here and needs a
     * console probe (see PR). */
    assert(CELL_GAME_ERROR_PARAM == (s32)0x8002CB07);
    assert(cellGamePatchCheck(&size, 0) == CELL_GAME_ERROR_NOTPATCH);
    assert(cellGamePatchCheck(NULL, 0) == CELL_GAME_ERROR_NOTPATCH);
    assert(cellGamePatchCheck(&size, 1) == CELL_GAME_ERROR_PARAM);
    assert(cellGamePatchCheck(&size, 0x10000100u) == CELL_GAME_ERROR_PARAM);
    assert(cellGamePatchCheck(NULL, 1) == CELL_GAME_ERROR_PARAM);
    /* untouched: the size is written only on a patch boot */
    assert(((unsigned char*)&size)[0] == 0xAB);
    puts("PASS: cellGamePatchCheck answers PARAM for a reserved argument, NOTPATCH otherwise");
    return 0;
}

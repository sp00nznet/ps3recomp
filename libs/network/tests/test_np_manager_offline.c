/* The NP manager's ticket, account-region and network-time imports
 * (sceNp.c): OFFLINE without a psnr server, as RPCS3's; with one, a ticket
 * request is answered at once with a failed GOT_TICKET (psnr issues no
 * tickets), and the region and the time are written. Unimplemented, they
 * answered CELL_OK with nothing written and no event ever came.
 *
 * clang -std=gnu17 -I include libs/network/tests/test_np_manager_offline.c
 *   -o /tmp/test_np_manager_offline && /tmp/test_np_manager_offline
 * (tools/test_np_manager_offline.py builds and runs it.)
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
static int s_psnr;
int np_psnr_enabled(void) { return s_psnr; }
const char* np_psnr_online_id(void) { return "PS3Player"; }
static unsigned queued;
static u32 queued_opd;
static u64 queued_args[8];
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8])
{
    queued++; queued_opd = opd;
    for (int i = 0; i < 8; i++) queued_args[i] = args[i];
    return 0;
}
void edat_add_klicensee(const uint8_t klic[16]) { (void)klic; }

#define NPID   0x1000u
#define SVC    0x1100u
#define BUF    0x1200u
#define SIZE   0x1300u
#define PARAM  0x1400u
#define CC     0x1500u
#define LANG   0x1504u
#define TICK   0x1600u

int main(void)
{
    vm_base = calloc(1, 65536);
    assert(vm_base);

    assert((u32)sceNpManagerRequestTicket2(NPID, 0, SVC, 0, 0, 0, 0) == SCE_NP_ERROR_NOT_INITIALIZED);
    assert(sceNpInit(131072, (void*)0x8000) == CELL_OK);
    assert(sceNpManagerRegisterCallback((SceNpManagerCallback)(uintptr_t)0x2000u, (void*)(uintptr_t)0x3000u) == CELL_OK);

    /* Argument checks first, online or not. */
    assert((u32)sceNpManagerRequestTicket2(0, 0, SVC, 0, 0, 0, 0) == SCE_NP_AUTH_EINVALID_ARGUMENT);
    assert((u32)sceNpManagerRequestTicket2(NPID, 0, SVC, 0, 1025, 0, 0) == SCE_NP_AUTH_EINVALID_ARGUMENT);
    assert((u32)sceNpManagerGetTicket(BUF, 0) == SCE_NP_ERROR_INVALID_ARGUMENT);
    assert((u32)sceNpManagerGetTicketParam(12, PARAM) == SCE_NP_ERROR_INVALID_ARGUMENT);
    assert((u32)sceNpManagerGetAccountRegion(CC, 0) == SCE_NP_ERROR_INVALID_ARGUMENT);
    assert((u32)sceNpManagerGetNetworkTime(0) == SCE_NP_ERROR_INVALID_ARGUMENT);

    /* Offline: OFFLINE, nothing written, no event. */
    vm_write32(LANG, 0xDEADBEEFu);
    assert((u32)sceNpManagerRequestTicket2(NPID, 0, SVC, 0, 0, 0, 0) == SCE_NP_ERROR_OFFLINE);
    assert(queued == 0);
    assert((u32)sceNpManagerGetAccountRegion(CC, LANG) == SCE_NP_ERROR_OFFLINE);
    assert(vm_read32(LANG) == 0xDEADBEEFu);
    assert((u32)sceNpManagerGetNetworkTime(TICK) == SCE_NP_ERROR_OFFLINE);
    /* There is never a ticket: its size is 0 and its fields are not there. */
    vm_write32(SIZE, 64);
    assert(sceNpManagerGetTicket(BUF, SIZE) == CELL_OK && vm_read32(SIZE) == 0);
    assert((u32)sceNpManagerGetTicketParam(0, PARAM) == SCE_NP_ERROR_INVALID_STATE);

    /* Signed in to psnr: the request is answered at once through the
     * manager callback, with an error instead of a ticket size. */
    s_psnr = 1;
    queued = 0;
    assert(sceNpManagerRequestTicket2(NPID, 0, SVC, 0, 0, 0, 0) == CELL_OK);
    assert(queued == 1 && queued_opd == 0x2000u);
    assert(queued_args[0] == SCE_NP_MANAGER_EVENT_GOT_TICKET);
    assert((s32)(u32)queued_args[1] < 0);
    assert(queued_args[2] == 0x3000u);
    assert(sceNpManagerGetAccountRegion(CC, LANG) == CELL_OK);
    assert(vm_base[CC] == 'u' && vm_base[CC + 1] == 's' && vm_base[CC + 2] == 0);
    assert(vm_read32(LANG) == 1);
    assert(sceNpManagerGetNetworkTime(TICK) == CELL_OK);
    /* After 2020-01-01 (63713433600 s after 0001-01-01), in microseconds. */
    assert(vm_read64(TICK) > 63713433600ull * 1000000ull);

    assert(sceNpTerm() == CELL_OK);
    free(vm_base);
    puts("NP manager offline/psnr checks passed");
    return 0;
}

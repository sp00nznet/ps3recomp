/*
 * test_netctl_netstart - the result the net-start dialog ends with.
 *
 *   cc -std=gnu17 -I include -I libs/system -o test_netctl_netstart libs/network/tests/test_netctl_netstart.c
 *
 * Offline the dialog ends cancelled (CELL_NET_CTL_ERROR_DIALOG_CANCELED, as
 * RPCS3 reports it without a network), so a title does not go on to the PSN
 * steps GetState already said are unreachable; PS3_NET_ONLINE=1 ends it
 * connected. The result word is big-endian in guest memory. Run it with and
 * without PS3_NET_ONLINE=1.
 */
#include "../cellNetCtl.c"

#include <assert.h>

uint8_t* vm_base;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static int s_unloaded;
void cellSysutilQueueEvent(int slot, u32 status, u32 param)
{
    (void)slot; (void)param;
    if (status == CELL_SYSUTIL_NET_CTL_NETSTART_UNLOADED) s_unloaded++;
}
const char* np_psnr_identity(void) { return "PS3Player"; }

static u32 be32_at(u32 ea)
{
    const uint8_t* p = vm_base + ea;
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

int main(void)
{
    static uint8_t mem[0x10000];
    vm_base = mem;
    const u32 ea = 0x1000;            /* CellNetCtlNetStartDialogResult: size, result */

    const int online = getenv("PS3_NET_ONLINE") != NULL;
    mem[ea + 4] = 0xAA;               /* the result word is written, not left */
    assert(cellNetCtlNetStartDialogUnloadAsync((CellNetCtlNetStartDialogResult*)(uintptr_t)ea) == CELL_OK);
    if (online) assert(be32_at(ea + 4) == 0u);
    else        assert(be32_at(ea + 4) == 0x80130190u);   /* CELL_NET_CTL_ERROR_DIALOG_CANCELED */
    assert(s_unloaded == CELL_SYSUTIL_MAX_CALLBACKS);
    printf("%s: result 0x%08X\n", online ? "PS3_NET_ONLINE=1" : "offline", be32_at(ea + 4));
    return 0;
}

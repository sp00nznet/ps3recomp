/* clang -std=gnu17 -I include libs/network/tests/test_trophy_registration.c
 * libs/system/cellSysutil.c -Wl,-dead_strip -pthread -o /tmp/test_trophy_registration
 */
#include "../sceNpTrophy.c"
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
static unsigned calls;
static void callback(u32 opd, u64 context, u64 status, u64 completed, u64 total,
                     u64 arg, u64 a, u64 b, u64 c)
{
    assert(opd == 0x300 && context == 1 && arg == 0xCAFE);
    assert(!completed && !total && !a && !b && !c);
    assert(status == (calls ? 8 : 3)); calls++;
}
int main(void)
{
    char root[] = "/tmp/ps3recomp-trophy-XXXXXX";
    assert(mkdtemp(root));
    vm_base = calloc(1, 65536); assert(vm_base);
    strcpy((char*)vm_base + 0x400, root);
    sceNpTrophySetStoragePath((void*)0x400);
    memcpy(vm_base + 0x100, "NPWR00000", 10);
    assert(sceNpTrophyInit(NULL, 0, 0, 0) == CELL_OK);
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, NULL, 0) == CELL_OK);
    assert(sceNpTrophyCreateHandle((void*)0x204) == CELL_OK);
    assert(sceNpTrophyRegisterContext(1, 1, 0x300, 0xCAFE, 0) == SCE_NP_TROPHY_ERROR_UNKNOWN);
    g_ps3_guest_caller = callback;
    assert(sceNpTrophyRegisterContext(1, 1, 0x300, 0xCAFE, 0) == CELL_OK);
    assert(calls == 1 && s_contexts[1].registered);
    cellSysutilCheckCallback(); assert(calls == 2);
    cellSysutilCheckCallback(); assert(calls == 2);
    assert(sceNpTrophyTerm() == CELL_OK);
    char file[256]; snprintf(file, sizeof(file), "%s/NPWR00000.json", root);
    assert(unlink(file) == 0); assert(rmdir(root) == 0); free(vm_base);
    puts("Trophy registration delivers installed and deferred completion statuses");
}

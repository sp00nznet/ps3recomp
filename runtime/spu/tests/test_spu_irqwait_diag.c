/* SPU_IRQWAIT_DIAG: an SPU with interrupts enabled and nothing pending reports
 * itself every 2^22 such dispatches, and with the variable unset the gate the
 * dispatch loop reads settles at 0 after the first call. Usage:
 *   test_spu_irqwait_diag on <log>    (SPU_IRQWAIT_DIAG=1 SPU_IRQWAIT_LS=12A0)
 *   test_spu_irqwait_diag off         (neither set) */
#include "spu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;

/* The runtime services the linked SPU sources name; none runs here. */
void ppu_resv_break_store(uint64_t ea) { (void)ea; abort(); }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
unsigned long long ps3_ms_now(void) { abort(); }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi; int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{ (void)c; (void)a; (void)b; (void)d; abort(); }
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }

static uint8_t s_ls[SPU_LS_SIZE];
static spu_context s_ctx;

int main(int argc, char** argv)
{
    int on = argc > 1 && strcmp(argv[1], "on") == 0;
    s_ctx.ls = s_ls;
    s_ctx.pc = 0x1234;
    s_ctx.int_enable = 1;
    s_ctx.event_mask = 0x2;           /* waiting for Sn ... */
    s_ctx.event_status = 0;           /* ... which never arrives */
    s_ctx.list_stall_mask = 0x10;
    s_ls[0x12A0] = 0xAB; s_ls[0x12AF] = 0xCD;
    if (on && !freopen(argv[2], "w", stderr)) return 2;

    const unsigned long long n = 3ull << 22;
    for (unsigned long long i = 0; i < n; i++) spu_irqwait_diag(&s_ctx);

    if (!on) {
        int ok = g_spu_irqwait_diag == 0;
        printf("%s: gate %d with SPU_IRQWAIT_DIAG unset\n", ok ? "PASS" : "FAIL", g_spu_irqwait_diag);
        return ok ? 0 : 1;
    }
    fclose(stderr);
    FILE* f = fopen(argv[2], "r");
    char line[512];
    int lines = 0, good = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (strncmp(line, "[irq-wait]", 10)) continue;
        lines++;
        good += strstr(line, "pc=0x01234") && strstr(line, "status=0x0 mask=0x2") &&
                strstr(line, "list_stall_mask=0x10") &&
                strstr(line, "LS[012A0]=AB000000 00000000 00000000 000000CD") != NULL;
    }
    if (f) fclose(f);
    int ok = lines == 3 && good == 3 && g_spu_irqwait_diag == 1;
    printf("%s: %d report(s) for %llu dispatches, %d with the expected state\n",
           ok ? "PASS" : "FAIL", lines, n, good);
    return ok ? 0 : 1;
}

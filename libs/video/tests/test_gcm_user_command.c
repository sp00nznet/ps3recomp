/* clang -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_user_command.c -Wl,-dead_strip -pthread
 *   -o /tmp/test_gcm_user_command && /tmp/test_gcm_user_command
 */
#include "../cellGcmSys.c"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
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
static uint32_t last;
static atomic_int paused, resume_pump;
static void* pump_thread(void* unused)
{
    (void)unused; ppu_gcm_pump(); return NULL;
}
static void callback(uint32_t opd, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f, uint64_t h, uint64_t i)
{
    if (opd == 0x300) {
        atomic_store(&paused, 1);
        while (!atomic_load(&resume_pump)) sched_yield();
        return;
    }
    if (opd == 0x200) { cellGcmQueueUserCommand(0xDD); return; }
    assert(opd == 0x100 && !b && !c && !d && !e && !f && !h && !i);
    calls++; last = (uint32_t)a;
    if (a == 0xAA) { cellGcmQueueUserCommand(0xBB); ppu_gcm_pump(); }
    return;
}
int main(void)
{
    g_ps3_guest_caller = callback;
    cellGcmSetUserHandler((CellGcmUserHandler)(uintptr_t)0x100);
    cellGcmQueueUserCommand(1);
    cellGcmQueueUserCommand(0xAA);
    assert(calls == 0);
    ppu_gcm_pump();
    assert(calls == 1 && last == 0xAA);
    ppu_gcm_pump();
    assert(calls == 2 && last == 0xBB);
    ppu_gcm_pump(); assert(calls == 2);
    cellGcmQueueUserCommand(0); ppu_gcm_pump();
    assert(calls == 3 && last == 0);
    /* A vblank callback can publish another cause after this pump has
     * claimed the old interrupt. It must not replace that claimed cause. */
    s_vblank_handler_opd = 0x200;
    cellGcmQueueUserCommand(0xCC);
    cellGcmTickVBlank();
    ppu_gcm_pump(); assert(calls == 4 && last == 0xCC);
    s_vblank_handler_opd = 0;
    ppu_gcm_pump(); assert(calls == 5 && last == 0xDD);
    ppu_gcm_pump(); assert(calls == 5);
    /* While one host thread has claimed an interrupt but is still in a
     * vblank callback, another pump must not deliver a newer cause first. */
    pthread_t worker;
    s_vblank_handler_opd = 0x300;
    cellGcmQueueUserCommand(0xE1);
    cellGcmTickVBlank();
    assert(pthread_create(&worker, NULL, pump_thread, NULL) == 0);
    while (!atomic_load(&paused)) sched_yield();
    cellGcmQueueUserCommand(0xE2);
    ppu_gcm_pump();
    assert(calls == 5);
    atomic_store(&resume_pump, 1);
    assert(pthread_join(worker, NULL) == 0);
    assert(calls == 6 && last == 0xE1);
    s_vblank_handler_opd = 0;
    ppu_gcm_pump(); assert(calls == 7 && last == 0xE2);
    ppu_gcm_pump(); assert(calls == 7);
    cellGcmSetUserHandler(NULL);
    cellGcmQueueUserCommand(7); ppu_gcm_pump(); assert(calls == 7);
    puts("Deferred GCM user-command checks passed");
}

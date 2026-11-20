/* GCM_HANDLER_THREAD=1 delivers the guest's vblank/flip/user handlers on a dedicated interrupt thread. They are guest
 * code, so that thread needs the host stack a guest PPU thread gets (256 MB, sys_ppu_thread.c): it was started with
 * 1 MB, and a handler whose call chain went deeper than that overflowed it.
 *
 * Includes the real implementation, registers a vblank handler, marks a vblank pending and pumps from the main thread
 * (which only starts the interrupt thread). The handler checks it is not on the main thread, reads its own stack
 * size, and then runs a 16 MB deep call chain, which a 1 MB stack cannot hold. */
#include "../libs/video/cellGcmSys.c"
#include <assert.h>
#include <pthread.h>
#include "ps3emu/guest_host_thread.h"   /* PS3_GUEST_HOST_STACK_BYTES */

uint8_t* vm_base;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
int g_resv_store_active;
void ppu_resv_break_store(uint64_t addr) { (void)addr; abort(); }
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t addr) { (void)addr; abort(); }
void spu_lockline_lock(void) { abort(); }
void spu_lockline_unlock(void) { abort(); }
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);

static unsigned long s_main_tid;
static volatile unsigned long s_handler_tid;
static volatile size_t s_handler_stack;
static volatile int s_handler_done;

static size_t my_stack_size(void)
{
#if defined(__APPLE__)
    return pthread_get_stacksize_np(pthread_self());
#else
    pthread_attr_t a;
    size_t sz = 0;
    if (pthread_getattr_np(pthread_self(), &a) != 0) return 0;
    pthread_attr_getstacksize(&a, &sz);
    pthread_attr_destroy(&a);
    return sz;
#endif
}

/* 64 KB of frame per level, 256 levels: a 16 MB chain, as a deep guest handler makes on the host stack. */
static __attribute__((noinline)) unsigned deep(unsigned n)
{
    volatile unsigned char frame[64 * 1024];
    frame[0] = (unsigned char)n;
    frame[sizeof frame - 1] = (unsigned char)n;
    return n ? deep(n - 1) + frame[0] + frame[sizeof frame - 1] : 0;
}

static void vblank_handler(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)opd; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (s_handler_done) return;
    s_handler_tid = (unsigned long)GetCurrentThreadId();
    s_handler_stack = my_stack_size();
    (void)deep(256);
    s_handler_done = 1;
}
ps3_guest_caller_fn g_ps3_guest_caller = vblank_handler;

int main(void)
{
    setenv("GCM_HANDLER_THREAD", "1", 1);
    s_main_tid = (unsigned long)GetCurrentThreadId();
    s_vblank_handler_opd = 0x1000;
    GCM_PENDING_SET(1);
    ppu_gcm_pump();   /* not the interrupt thread: starts it and returns */
    for (int i = 0; i < 5000 && !s_handler_done; i++) usleep(1000);

    int fails = 0;
    if (!s_handler_done) { fprintf(stderr, "FAIL: the vblank handler never ran\n"); return 1; }
    if (s_handler_tid == s_main_tid) { fprintf(stderr, "FAIL: the handler ran on the main thread\n"); fails++; }
    if (s_handler_stack < PS3_GUEST_HOST_STACK_BYTES) {
        fprintf(stderr, "FAIL: the interrupt thread has a %zu KB stack, want the %u KB a PPU thread gets\n",
                (size_t)s_handler_stack / 1024, PS3_GUEST_HOST_STACK_BYTES / 1024);
        fails++;
    }
    if (fails) return 1;
    printf("PASS: GCM handlers run on an interrupt thread with a guest PPU thread's host stack\n");
    return 0;
}

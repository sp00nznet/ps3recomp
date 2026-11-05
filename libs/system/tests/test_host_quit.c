/* macOS: clang -std=gnu17 -I include libs/system/tests/test_host_quit.c
 * libs/system/cellSysutil.c -Wl,-dead_strip -pthread -o /tmp/test_host_quit && /tmp/test_host_quit
 * (tools/test_host_quit.py runs it on macOS and Linux.)
 *
 * ps3_host_quit (cellSysutil.c): every registered sysutil callback gets
 * REQUEST_EXITGAME on the next poll; a title that does not exit by itself is
 * ended with status 0 after PS3_QUIT_GRACE_MS, but not while a save is being
 * written. The process exits from the quit thread, so the checks that need
 * the end run in an atexit handler; main returning means it never ended. */
#include "../cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
void np_psnr_pump(void) {}
void cellMsgDialog_tick(void) {}
void ps3_np_eula_tick(void) {}

static volatile int s_busy = 1;       /* a save is being written */
static volatile int s_busy_cleared;   /* ...and it has finished */
int ps3_savedata_busy(void) { return s_busy; }

static volatile unsigned s_exitgame, s_other;
static volatile u64 s_userdata;
static void caller(u32 opd, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h)
{
    (void)b; (void)d; (void)e; (void)f; (void)g; (void)h;
    if (opd == 0x100 && a == CELL_SYSUTIL_REQUEST_EXITGAME) { s_exitgame++; s_userdata = c; }
    else s_other++;
}

static void at_end(void)
{
    const int ok = s_exitgame == 1 && s_userdata == 0x4444 && s_other == 0 && s_busy_cleared;
    printf("[%s] REQUEST_EXITGAME delivered once (%u, userdata 0x%llX), exit only after the save "
           "finished (%d)\n", ok ? "PASS" : "FAIL", s_exitgame, (unsigned long long)s_userdata,
           s_busy_cleared);
    fflush(stdout);
    if (!ok) _exit(1);
}

static unsigned long long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    g_ps3_guest_caller = caller;
    if (cellSysutilRegisterCallback(0, (CellSysutilCallback)(uintptr_t)0x100u,
                                    (void*)(uintptr_t)0x4444u) != CELL_OK) {
        puts("[FAIL] register"); return 1;
    }
    atexit(at_end);
    if (ps3_host_quitting()) { puts("[FAIL] quitting before asked"); return 1; }
    ps3_host_quit("test");
    ps3_host_quit("test again");          /* ignored */
    if (!ps3_host_quitting()) { puts("[FAIL] not quitting after asked"); return 1; }

    /* The title keeps polling and never exits by itself. The grace is 100 ms
     * (tools/test_host_quit.py); the save finishes at 400 ms. */
    const unsigned long long t0 = ms_now();
    while (ms_now() - t0 < 5000) {
        cellSysutilCheckCallback();
        if (s_busy && ms_now() - t0 >= 400) { s_busy_cleared = 1; s_busy = 0; }
        usleep(10000);
    }
    puts("[FAIL] the process was not ended");
    _exit(1);
}

/* The SPURS coordinator is woken when an SPU commits the instance's request line
 * (the first 128 bytes: ready counts, workload flag, signal bits), not left to
 * find the request on its next timed pass. Runs against the real
 * libs/spurs/cellSpurs.c; no timing is measured: a missing wake leaves the
 * waiter below asleep forever and the driver's timeout fails the test. */
#include <ps3emu/ps3types.h>
#include "../libs/spurs/cellSpurs.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x20000;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
int g_resv_store_active;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void sys_event_queue_cancel_by_id(u32 id) { (void)id; }
void sys_event_queue_uncancel_by_id(u32 id) { (void)id; }

extern void (*g_spu_line_commit_hook)(uint32_t);

#define SPURS_EA 0x10000u   /* 128-byte aligned, as CellSpurs is */

static struct SpursInst* si;
static volatile int waiting, woken;

static DWORD WINAPI waiter(LPVOID p)
{
    (void)p;
    AcquireSRWLockExclusive(&si->lock);
    waiting = 1;
    while (!__atomic_load_n(&si->kick, __ATOMIC_ACQUIRE))
        SleepConditionVariableSRW(&si->wake, &si->lock, INFINITE, 0);
    woken = 1;
    ReleaseSRWLockExclusive(&si->lock);
    return 0;
}

static int take_kick(void) { return __atomic_exchange_n(&si->kick, 0, __ATOMIC_ACQ_REL); }

int main(void)
{
    si = &s_inst[0];
    InitializeSRWLock(&si->lock);
    InitializeConditionVariable(&si->wake);
    __atomic_store_n(&si->ea, SPURS_EA, __ATOMIC_RELEASE);
    si->kernel_live = 1;

    /* An event flag allocated after the scheduler started must not take the
     * line-commit hook back from the coordinator wake. */
    g_spu_line_commit_hook = 0;
    assert(ef_sync_alloc(0x20000u));
    assert(g_spu_line_commit_hook == spurs_commit_hook);

    /* Only a commit to the instance's first line kicks it. */
    g_spu_line_commit_hook(SPURS_EA);
    assert(take_kick() == 1);
    g_spu_line_commit_hook(SPURS_EA + 0x80u);
    g_spu_line_commit_hook(SPURS_EA - 0x80u);
    g_spu_line_commit_hook(0x20000u);
    assert(take_kick() == 0);

    /* A stopped instance is left alone. */
    si->kernel_live = 0;
    g_spu_line_commit_hook(SPURS_EA);
    assert(take_kick() == 0);
    si->kernel_live = 1;

    /* The commit wakes a thread sleeping on the coordinator's condition with no
     * timeout. Taking the lock after `waiting` is set proves the waiter is
     * inside the wait (it releases the lock only by sleeping), so the wake
     * cannot fall before it; a missing wake hangs here. */
    HANDLE t = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
    assert(t);
    for (;;) {
        AcquireSRWLockExclusive(&si->lock);
        int w = waiting;
        ReleaseSRWLockExclusive(&si->lock);
        if (w) break;
        Sleep(0);
    }
    AcquireSRWLockExclusive(&si->lock);   /* waiter is now asleep */
    ReleaseSRWLockExclusive(&si->lock);
    g_spu_line_commit_hook(SPURS_EA);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    assert(woken);

    puts("PASS: an SPU commit to the SPURS request line kicks and wakes the coordinator");
    return 0;
}

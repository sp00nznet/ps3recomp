/* sys_ppu_thread_join / detach through the real sys_ppu_thread.c, with real host threads.
 *
 *   - A thread that ends in sys_ppu_thread_exit signals its joiner from inside the thread body and then still unwinds
 *     through the host thread's epilogue. On Windows the joiner freed the slot and closed the handles at once; the
 *     epilogue then wrote to the closed handle and put the slot it had just freed back to FINISHED, where it stayed
 *     for good. The join now waits for the host thread to end first, so after a join (and after a detach of a
 *     finished thread) the slot is FREE and stays FREE.
 *   - Joining yourself is EDEADLK; a second joiner of the same thread is EINVAL.
 *
 * The guest entry is a trampoline that exits, returns, or waits for a gate, by its argument. */
#include "../../runtime/syscalls/sys_ppu_thread.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x10000;
uint32_t vm_read32(uint64_t a)         /* guest words are big-endian (sys_ppu_thread_yield peeks at some) */
{
    const uint8_t* p = vm_base + (uint32_t)a;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
void ppu_resv_register(ppu_context* c) { (void)c; }
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }

enum { ARG_RETURN = 0, ARG_EXIT = 1, ARG_GATED = 2 };
static volatile int s_gate;            /* a gated thread returns once this is set */

static void guest_entry(ppu_context* ctx)
{
    switch ((int)ctx->gpr[3]) {
    case ARG_EXIT:
        ctx->gpr[3] = 9;               /* exit status */
        sys_ppu_thread_exit(ctx);      /* does not return when the unwind is armed */
        break;
    case ARG_GATED:
        while (!s_gate) Sleep(1);
        ctx->gpr[3] = 3;
        break;
    default:
        ctx->gpr[3] = 5;
        break;
    }
}

static int g_fails;
#define EXPECT_EQ(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); if (g_ != w_) { g_fails++; \
    fprintf(stderr, "FAIL line %d: %s = 0x%llX, want 0x%llX\n", __LINE__, #got, g_, w_); } } while (0)

static int64_t create(uint64_t flags, uint64_t arg, uint64_t* tid)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = 0x100;            /* tid out */
    ctx.gpr[4] = 0x10000;          /* entry */
    ctx.gpr[5] = arg;
    ctx.gpr[6] = 1000;             /* priority */
    ctx.gpr[7] = 0x10000;          /* stack size */
    ctx.gpr[8] = flags;
    ctx.gpr[9] = 0;                /* name */
    int64_t rc = sys_ppu_thread_create(&ctx);
    if (rc == 0 && tid) { uint64_t be; memcpy(&be, vm_base + 0x100, 8); *tid = __builtin_bswap64(be); }
    return rc;
}
static int64_t join_as(uint64_t caller, uint64_t tid, int64_t* status)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.thread_id = caller;
    ctx.gpr[3] = tid; ctx.gpr[4] = 0x200;
    int64_t rc = sys_ppu_thread_join(&ctx);
    if (rc == 0 && status) { uint64_t be; memcpy(&be, vm_base + 0x200, 8); *status = (int64_t)__builtin_bswap64(be); }
    return rc;
}
static int64_t detach(uint64_t tid)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = tid;
    return sys_ppu_thread_detach(&ctx);
}
static int slot_state(uint64_t tid) { table_lock(); int s = g_ppu_threads[tid - 1].state; table_unlock(); return s; }
static int used_slots(void)            /* slots other than main that are not FREE */
{
    int n = 0;
    table_lock();
    for (int i = 1; i < PPU_THREAD_MAX; i++) if (g_ppu_threads[i].state != PPU_THREAD_STATE_FREE) n++;
    table_unlock();
    return n;
}
static void wait_state(uint64_t tid, int want)
{
    for (int i = 0; i < 5000 && slot_state(tid) != want; i++) Sleep(1);
}

typedef struct { uint64_t caller, tid; int64_t rc, status; } bgjoin_t;
static DWORD WINAPI bgjoin_main(LPVOID p)
{
    bgjoin_t* b = (bgjoin_t*)p;
    b->rc = join_as(b->caller, b->tid, &b->status);
    return 0;
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 0x10000); assert(vm_base);
    g_vm_stack_alloc.next_addr = 0x10000000u;   /* the guest stack region the threads bump-allocate from */
    g_vm_stack_alloc.region_end = 0xA0000000u;
    g_ppu_thread_entry_trampoline = guest_entry;
    ppu_thread_register_main();

    /* 1. Create / exit / join, over and over: every join returns the exit status and leaves its slot FREE for good.
     *    The slot table has 64 entries and is searched next-fit, so 200 rounds reuse every slot several times. */
    for (int i = 0; i < 200; i++) {
        uint64_t tid = 0; int64_t status = 0;
        EXPECT_EQ(create(1, (i & 1) ? ARG_EXIT : ARG_RETURN, &tid), 0);
        EXPECT_EQ(join_as(1, tid, &status), 0);
        EXPECT_EQ(status, (i & 1) ? 9 : 5);
        Sleep(0);                       /* give a late epilogue its chance to misbehave */
    }
    Sleep(50);
    EXPECT_EQ(used_slots(), 0);         /* nothing resurrected as FINISHED */

    /* 2. Detaching a thread that has exited: the slot is freed and stays free. */
    for (int i = 0; i < 100; i++) {
        uint64_t tid = 0;
        EXPECT_EQ(create(1, ARG_EXIT, &tid), 0);
        wait_state(tid, PPU_THREAD_STATE_FINISHED);
        EXPECT_EQ(detach(tid), 0);
    }
    Sleep(50);
    EXPECT_EQ(used_slots(), 0);

    /* 3. Self-join and a second joiner. */
    {
        uint64_t tid = 0; int64_t status = 0;
        s_gate = 0;
        EXPECT_EQ(create(1, ARG_GATED, &tid), 0);
        EXPECT_EQ(join_as(tid, tid, &status), (int64_t)(int32_t)CELL_EDEADLK);   /* the thread joining itself */
        bgjoin_t b = { 1, tid, -1, 0 };
        HANDLE h = CreateThread(NULL, 0, bgjoin_main, &b, 0, NULL);
        for (int i = 0; i < 5000; i++) {
            table_lock(); int joining = g_ppu_threads[tid - 1].joining; table_unlock();
            if (joining) break;
            Sleep(1);
        }
        EXPECT_EQ(join_as(1, tid, &status), (int64_t)(int32_t)CELL_EINVAL);       /* a second joiner */
        s_gate = 1;
        WaitForSingleObject(h, 20000); CloseHandle(h);
        EXPECT_EQ(b.rc, 0);
        EXPECT_EQ(b.status, 3);
        Sleep(50);
        EXPECT_EQ(used_slots(), 0);
        EXPECT_EQ(join_as(1, tid, &status), (int64_t)(int32_t)CELL_ESRCH);        /* and it is gone */
    }

    printf("ppu thread join: %s\n", g_fails ? "FAIL" : "PASS");
    return g_fails ? 1 : 0;
}

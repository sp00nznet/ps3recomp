/* lv2 slot 41 is _sys_ppu_thread_exit and slot 42 is the shared ENOSYS stub (FW 4.93 stock syscall table; RPCS3 at
 * pin d00d8023, lv2.cpp, binds _sys_ppu_thread_exit at 41 and uns_func at 42). Thread creation is 52/53 on lv2 with
 * another ABI, so it is not a table entry here: it is reached by NID.
 *
 * Runs the table sys_ppu_thread_init builds through the path a guest `sc`
 * takes: lv2_syscall()'s default branch consults lv2_try_syscall() (real
 * lv2_register.c, linked) and, only for a genuinely unregistered number,
 * writes the CELL_OK fallback. A guest that issues `sc` 41 with the firmware
 * ABI (r3 = the status) must end there, even when r4..r9 still hold what
 * happens to be a valid sys_ppu_thread_create from an earlier call: on the old
 * table slot 41 was create, so that `sc` started a foreign thread from the
 * stale registers and returned CELL_OK to a caller that meant to exit. `sc`
 * 42 must answer CELL_ENOSYS and leave the caller running -- and must do so
 * because slot 42 holds a real handler, not because the fallback was reached:
 * the fallback writes CELL_OK. `sc` 53 (unregistered) pins the fallback. */
#include "../runtime/syscalls/sys_ppu_thread.c"
#include <assert.h>
#include <time.h>

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x10000;
/* Only sys_ppu_thread_yield's opt-in diagnostics read it, and sys_ppu_thread_init registers yield; no case here
 * enables one. */
uint32_t vm_read32(uint64_t a) { uint32_t v; memcpy(&v, vm_base + (uint32_t)a, 4); return v; }
void ppu_resv_register(ppu_context* c) { (void)c; }
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }

#define ARG_SC41      1
#define ARG_SC42      2
#define ARG_SC53      3
#define ARG_SPURIOUS  4
#define EXIT_CODE_41  0x4C1ull
#define JOINABLE      1u

static int      g_returned41;             /* set only if sc 41 came back, which it never does */
static int      g_spurious_ran;           /* a thread started from the stale registers */
static int      g_returned42;
static uint64_t g_rc42;                   /* r3 after sc 42 */
static int      g_returned53;
static uint64_t g_rc53;                   /* r3 after sc 53 */

/* lv2_syscall()'s default branch, without the harness around it. */
static void guest_sc(ppu_context* ctx)
{
    if (!lv2_try_syscall(ctx))
        ctx->gpr[3] = 0;                  /* CELL_OK fallback for unregistered numbers */
}

static void guest_entry(ppu_context* ctx)
{
    const uint64_t arg = ctx->gpr[3];
    if (arg == ARG_SC41) {
        /* _sys_ppu_thread_exit(status): r11 = 41, r3 = the status. r4..r9 are valid sys_ppu_thread_create arguments
         * (entry, argument, priority, stack size, flags, name), as if the previous call had been one. */
        ctx->gpr[11] = 41;
        ctx->gpr[3]  = EXIT_CODE_41;
        ctx->gpr[4]  = 0x10000;
        ctx->gpr[5]  = ARG_SPURIOUS;
        ctx->gpr[6]  = 1000;
        ctx->gpr[7]  = 0x10000;
        ctx->gpr[8]  = JOINABLE;
        ctx->gpr[9]  = 0;
        guest_sc(ctx);
        g_returned41 = 1;
        ctx->gpr[3] = 0xBAD41;
        return;
    }
    if (arg == ARG_SPURIOUS) {
        __atomic_store_n(&g_spurious_ran, 1, __ATOMIC_RELEASE);
        ctx->gpr[3] = 0;
        return;
    }
    if (arg == ARG_SC42) {
        ctx->gpr[11] = 42;
        ctx->gpr[3]  = 0x77;
        guest_sc(ctx);
        g_rc42 = ctx->gpr[3];
        g_returned42 = 1;
        ctx->gpr[3] = 0x42;               /* the thread runs on and ends with this status */
        return;
    }
    if (arg == ARG_SC53) {
        ctx->gpr[11] = 53;                /* sys_ppu_thread_start: unregistered -> the CELL_OK fallback */
        ctx->gpr[3]  = 0x77;
        guest_sc(ctx);
        g_rc53 = ctx->gpr[3];
        g_returned53 = 1;
        ctx->gpr[3] = 0x53;               /* the thread runs on and ends with this status */
        return;
    }
    ctx->gpr[3] = 0;
}

static int g_fails;
#define EXPECT_EQ(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); if (g_ != w_) { g_fails++; \
    fprintf(stderr, "FAIL line %d: %s = 0x%llX, want 0x%llX\n", __LINE__, #got, (unsigned long long)g_, \
            (unsigned long long)w_); } } while (0)

static int64_t create_thread(uint64_t arg, uint64_t* tid_out)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = 0x100;            /* tid out */
    ctx.gpr[4] = 0x10000;          /* entry */
    ctx.gpr[5] = arg;
    ctx.gpr[6] = 1000;             /* priority */
    ctx.gpr[7] = 0x10000;          /* stack size */
    ctx.gpr[8] = JOINABLE;
    ctx.gpr[9] = 0;                /* name */
    int64_t rc = sys_ppu_thread_create(&ctx);
    if (rc == 0) { uint64_t be; memcpy(&be, vm_base + 0x100, 8); *tid_out = __builtin_bswap64(be); }
    return rc;
}

static int64_t join_thread(uint64_t tid, uint64_t* status)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.thread_id = 1;             /* the main thread */
    ctx.gpr[3] = tid;
    ctx.gpr[4] = 0x200;
    int64_t rc = sys_ppu_thread_join(&ctx);
    if (rc == 0) { uint64_t be; memcpy(&be, vm_base + 0x200, 8); *status = __builtin_bswap64(be); }
    return rc;
}

/* Slots other than main that are not FREE: a thread nobody has joined keeps its slot. */
static int used_slots(void)
{
    int n = 0;
    table_lock();
    for (int i = 1; i < PPU_THREAD_MAX; i++) if (g_ppu_threads[i].state != PPU_THREAD_STATE_FREE) n++;
    table_unlock();
    return n;
}

int main(void)
{
    vm_base = calloc(1, 0x10000); assert(vm_base);
    lv2_syscall_table_init(&g_lv2_syscalls);
    sys_ppu_thread_init(&g_lv2_syscalls);      /* clears the thread table and registers the handlers */
    g_vm_stack_alloc.next_addr = 0x10000000u;  /* the guest stack region the threads bump-allocate from */
    g_vm_stack_alloc.region_end = 0xA0000000u;
    g_ppu_thread_entry_trampoline = guest_entry;
    ppu_thread_register_main();

    /* The table itself: exit at 41, a real ENOSYS handler at 42, and no create at 41 or at lv2's 52/53. */
    EXPECT_EQ(g_lv2_syscalls.handlers[41] == sys_ppu_thread_exit, 1);
    EXPECT_EQ(g_lv2_syscalls.handlers[42] == sys_ppu_thread_enosys_stub, 1);
    EXPECT_EQ(g_lv2_syscalls.handlers[52] == lv2_syscall_unimplemented, 1);
    EXPECT_EQ(g_lv2_syscalls.handlers[53] == lv2_syscall_unimplemented, 1);

    /* sc 41 with the firmware ABI ends the caller with its status, never returns, and starts no thread. */
    uint64_t t = 0, status = 0;
    EXPECT_EQ(create_thread(ARG_SC41, &t), 0);
    EXPECT_EQ(join_thread(t, &status), 0);
    EXPECT_EQ(status, EXIT_CODE_41);           /* the status in r3, not a CELL_OK from a create */
    EXPECT_EQ(g_returned41, 0);
    for (int i = 0; i < 20; i++) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
    EXPECT_EQ(__atomic_load_n(&g_spurious_ran, __ATOMIC_ACQUIRE), 0);
    EXPECT_EQ(used_slots(), 0);                /* the joined thread is gone and no other took a slot */

    /* sc 42 is the ENOSYS stub: it answers CELL_ENOSYS through the guest path and the thread runs on. */
    EXPECT_EQ(create_thread(ARG_SC42, &t), 0);
    EXPECT_EQ(join_thread(t, &status), 0);
    EXPECT_EQ(g_returned42, 1);
    EXPECT_EQ(g_rc42, (uint64_t)(int64_t)(int32_t)CELL_ENOSYS);   /* 0xFFFFFFFF80010003, not the CELL_OK fallback */
    EXPECT_EQ(status, 0x42);

    /* sc 53 is unregistered: the same path falls back to CELL_OK. */
    EXPECT_EQ(create_thread(ARG_SC53, &t), 0);
    EXPECT_EQ(join_thread(t, &status), 0);
    EXPECT_EQ(g_returned53, 1);
    EXPECT_EQ(g_rc53, 0);                      /* lv2_syscall()'s fallback */
    EXPECT_EQ(status, 0x53);

    free(vm_base);
    if (g_fails) { fprintf(stderr, "%d check(s) failed\n", g_fails); return 1; }
    puts("PASS: sc 41 is _sys_ppu_thread_exit and ends the caller; sc 42 answers CELL_ENOSYS; sc 53 answers the CELL_OK fallback");
    return 0;
}

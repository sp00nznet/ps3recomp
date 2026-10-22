/* sys_ppu_thread_get_stack_information through the real sys_ppu_thread.c. The main thread sits in the thread table
 * (ppu_thread_register_main, slot 0) with no recorded stack, so it used to be told 0 + 0, and a title that looks for
 * data at (addr + size - 0x1000) went to 0xFFFFF000. It now gets the host's g_ppu_main_stack_addr/size, or the
 * VM_STACK_BASE default; a created thread still gets its own stack. */
#include "../runtime/syscalls/sys_ppu_thread.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x10000;
/* The few runtime services the thread proc and create call; nothing here replaces the code under test. */
void ppu_resv_register(ppu_context* c) { (void)c; }
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }

static int g_fails;
#define EXPECT_EQ(got, want) do { long long g_ = (got), w_ = (want); if (g_ != w_) { g_fails++; \
    fprintf(stderr, "FAIL line %d: %s = 0x%llX, want 0x%llX\n", __LINE__, #got, g_, w_); } } while (0)

static uint32_t be32_at(uint32_t ea) { uint32_t v; memcpy(&v, vm_base + ea, 4); return __builtin_bswap32(v); }

/* Ask as the thread `ctx` is; the answer lands at `out` (two big-endian words). */
static void stack_info(ppu_context* ctx, uint32_t out, uint32_t* addr, uint32_t* size)
{
    memset(vm_base + out, 0xEE, 8);
    ctx->gpr[3] = out;
    EXPECT_EQ(sys_ppu_thread_get_stack_information(ctx), 0);
    *addr = be32_at(out);
    *size = be32_at(out + 4);
}

static volatile uint32_t g_child_addr, g_child_size;
static void guest_entry(ppu_context* ctx)        /* the created thread asks about itself, then returns */
{
    uint32_t a, s;
    stack_info(ctx, 0x200, &a, &s);
    g_child_addr = a; g_child_size = s;
    ctx->gpr[3] = 0;
}

int main(void)
{
    vm_base = calloc(1, 0x10000); assert(vm_base);
    g_vm_stack_alloc.next_addr = 0x10000000u;   /* the guest stack region the threads bump-allocate from */
    g_vm_stack_alloc.region_end = 0xA0000000u;
    g_ppu_thread_entry_trampoline = guest_entry;

    ppu_context main_ctx; memset(&main_ctx, 0, sizeof(main_ctx));
    main_ctx.thread_id = ppu_thread_register_main();
    EXPECT_EQ(main_ctx.thread_id, 1);
    uint32_t a, s;

    /* 1. Main, no host stack set: the VM_STACK_BASE default, not the 0 + 0 its table slot holds. */
    stack_info(&main_ctx, 0x100, &a, &s);
    EXPECT_EQ(a, VM_STACK_BASE);
    EXPECT_EQ(s, VM_PPU_STACK_SIZE);

    /* 2. Main, with the host's stack. */
    g_ppu_main_stack_addr = 0x0FE00000u;
    g_ppu_main_stack_size = 0x00100000u;
    stack_info(&main_ctx, 0x100, &a, &s);
    EXPECT_EQ(a, 0x0FE00000u);
    EXPECT_EQ(s, 0x00100000u);

    /* 3. A context with no table entry at all gets the main-thread answer too. */
    ppu_context stray; memset(&stray, 0, sizeof(stray));
    stray.thread_id = 0;
    stack_info(&stray, 0x100, &a, &s);
    EXPECT_EQ(a, 0x0FE00000u);
    EXPECT_EQ(s, 0x00100000u);

    /* 4. A created thread is told its own stack, whatever the host set for main. */
    {
        ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.gpr[3] = 0x300;            /* tid out */
        ctx.gpr[4] = 0x10000;          /* entry */
        ctx.gpr[6] = 1000;             /* priority */
        ctx.gpr[7] = 0x10000;          /* stack size */
        ctx.gpr[8] = 1;                /* joinable */
        EXPECT_EQ(sys_ppu_thread_create(&ctx), 0);
        uint64_t be; memcpy(&be, vm_base + 0x300, 8);
        ppu_context jctx; memset(&jctx, 0, sizeof(jctx));
        jctx.gpr[3] = __builtin_bswap64(be);
        EXPECT_EQ(sys_ppu_thread_join(&jctx), 0);
        EXPECT_EQ(g_child_size, 0x10000);
        EXPECT_EQ(g_child_addr >= 0x10000000u && g_child_addr < 0xA0000000u, 1);
    }

    free(vm_base);
    if (g_fails) { fprintf(stderr, "%d check(s) failed\n", g_fails); return 1; }
    puts("PASS: the main thread is told a real stack; a created thread is told its own");
    return 0;
}

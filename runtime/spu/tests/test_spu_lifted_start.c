/*
 * A thread group starts its threads on their image's lifted SPU code.
 *
 * The failure this guards is the one a real title hit: sys_spu_thread_group_start
 * looked up a PPU fallback and, failing that, the interpreter, and never asked
 * the lifted-code registry anything. Every thread of the group completed
 * instantly with status 0, the group reported a clean exit, and nothing
 * anywhere said that the program the guest asked to run had not run. Only the
 * absence of its effects, several layers downstream, ever showed.
 *
 * So the assertions here are about the things that are silent when they are
 * wrong. That each thread ran AT ALL, and ran on a host thread of its own
 * rather than one after another on the caller's -- checked with a barrier
 * inside the SPU code, so two threads that were serialized deadlock it and
 * time out instead of quietly passing. That each got its own 256 KB local
 * store, with the image's COPY segment really in it and the FILL segment
 * really filled. That the entry point and the image the registry dispatches in
 * are the ones the image declared. That each thread got ITS OWN arguments,
 * which is the failure mode when they are read at start time out of a guest
 * block the title rewrites between initialize calls. And that the status the
 * SPU wrote on the way out survives to the group's join.
 *
 * The real syscall handlers run: the file that defines them is compiled in, so
 * this drives sys_spu_thread_group_create, _initialize, _group_start and
 * _group_join themselves rather than a re-creation of what they do. What it
 * cannot bring is a guest, so the runtime the handlers reach for outside the
 * SPU and lv2 layers -- the PPU backtrace helpers, the other syscall families,
 * the event queues -- is stubbed at the bottom of this file.
 *
 * Build:
 *   clang -std=gnu17 -I include -I runtime/spu -I runtime/platform \
 *         -o /tmp/test_spu_lifted_start \
 *         runtime/spu/tests/test_spu_lifted_start.c \
 *         runtime/spu/spu_channels.c runtime/spu/spu_drain.c \
 *         runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c \
 *         runtime/spu/spu_workload.c runtime/spu/spurs_policy.c \
 *         runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c \
 *         runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c \
 *         runtime/spu/spu_interp.c runtime/spu/spu_lifted_thread.c \
 *         runtime/syscalls/spu_fallback.c runtime/syscalls/sdk_version_weak.c \
 *         runtime/platform/win32_compat.c \
 *         -lpthread
 *
 * Exit code: 0 if all passed, 1 if any failed. Final line prints a summary.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Guest memory. Defined before the unit under test is compiled in, because
 * that is what its vm_read_be32 reads through.
 *
 * A megabyte, which is more than every address this test hands the syscalls,
 * and an unreadable megabyte above it. It used to be 0x41000000 -- a gigabyte
 * of address space -- for one reason that had nothing to do with what is being
 * tested: the group-start path dumped a fixed guest address at 0x40009D00, one
 * title's SPURS instance, on every start of every group, and an arena that
 * stopped short of it failed on the diagnostic. That read is asked for by
 * environment now and bounds-checked against ppu_vm_size, so the arena can be
 * the size of the test again.
 *
 * The guard region above it is what keeps that honest. A real runtime maps
 * guest memory lazily, so a diagnostic reaching for an address the title never
 * allocated faults; here it faults too, by name, instead of reading whatever
 * the allocator happened to put next to us.
 * -----------------------------------------------------------------------*/
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#define GUEST_SIZE   0x00100000u   /* every address this test uses is below this */
#define GUEST_GUARD  0x00100000u   /* ...and nothing above it is readable        */
static uint8_t*  g_guest_mem;
uint8_t*         vm_base;
uint32_t         ppu_vm_size = GUEST_SIZE;

/* Darwin raises SIGBUS for an inaccessible mapping where Linux raises SIGSEGV
 * (runtime/platform/tests/test_guest_ptr_trap.c is where that was measured), so
 * both mean the same thing here. Naming the failure is the whole job; there is
 * nothing to carry on with afterwards, and write/_exit are what a handler may
 * call. */
static void guard_fault(int sig)
{
    static const char msg[] =
        "  FAIL: a guest read left the arena (the group-start diagnostics again?)\n"
        "SPU lifted thread-group start: FAILED\n";
    ssize_t ignored = write(2, msg, sizeof(msg) - 1);
    (void)ignored; (void)sig;
    _exit(1);
}

/* The lv2 SPU thread-group layer, handlers and all. Compiled in rather than
 * linked so the test can call the static syscall handlers directly and read
 * the group and thread tables to check what they did. */
#include "../../syscalls/lv2_register.c"
#include "../spu_helpers.h"
void spu_wrch(spu_context*, uint32_t, u128);
void spu_overlay_register_region(uint32_t, uint32_t, int);
void spu_overlay_note_get(spu_context*, uint32_t, const uint8_t*, uint32_t);

/* The lifted-code registry (spu_channels.c), which the test registers into. */
typedef void (*test_spu_fn)(spu_context*);
void spu_begin_image(int image_id);
void spu_register_function(uint32_t addr, test_spu_fn fn);
test_spu_fn spu_lookup(uint32_t addr, int image_id);
void spu_stop(spu_context* ctx);

/* ---------------------------------------------------------------------------
 * Harness
 * -----------------------------------------------------------------------*/
static int g_pass, g_fail;

static void check(int ok, const char* what)
{
    if (ok) { g_pass++; return; }
    g_fail++;
    printf("  FAIL: %s\n", what);
}

#define CHECK(cond)  check((cond), #cond)

/* ---------------------------------------------------------------------------
 * The synthetic SPU image
 *
 * Guest layout, mirroring what _sys_spu_image_import builds from an ELF: a
 * sys_spu_image { type, entry, segs, nsegs } pointing at a table of
 * sys_spu_segment { type, ls_start, size, src } records of 0x18 bytes each.
 * -----------------------------------------------------------------------*/
#define IMG_CODE_EA    0x00011000u   /* the COPY segment's source bytes      */
#define IMG_SEGS_EA    0x00012000u   /* the segment table                    */
#define IMG_EA         0x00013000u   /* the sys_spu_image struct             */
#define ARGS0_EA       0x00014000u   /* thread 0's sys_spu_thread_argument   */
#define ARGS1_EA       0x00014100u   /* thread 1's                           */
#define OUT_EA         0x00015000u   /* syscall out-parameter scratch        */

#define IMG_CODE_LS    0x800u        /* where the COPY segment lands in LS   */
#define IMG_CODE_LEN   0x40u
#define IMG_FILL_LS    0x840u        /* the FILL segment, as an ELF bss tail */
#define IMG_FILL_LEN   0x40u
#define IMG_FILL_BYTE  0xABu
#define IMG_ENTRY      0x848u        /* inside the filled region, on purpose */
#define IMG_IMAGE_ID   7             /* the id the lifted code registers under */

#define RESULT_LS      0x900u        /* where the SPU writes its result byte */
#define RESULT_BYTE    0x5Au

static void wr_be32(uint32_t ea, uint32_t v)
{
    uint8_t* p = g_guest_mem + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void wr_be64(uint32_t ea, uint64_t v)
{
    wr_be32(ea, (uint32_t)(v >> 32));
    wr_be32(ea + 4, (uint32_t)v);
}

static void build_guest_image(void)
{
    /* Code bytes the COPY segment must land in local store verbatim. */
    for (uint32_t i = 0; i < IMG_CODE_LEN; i++)
        g_guest_mem[IMG_CODE_EA + i] = (uint8_t)(0x40u + i);

    /* seg 0: COPY. _sys_spu_image_import writes the source EA at both +0x10
     * and +0x14; write both here for the same reason. */
    wr_be32(IMG_SEGS_EA + 0x00, 1);
    wr_be32(IMG_SEGS_EA + 0x04, IMG_CODE_LS);
    wr_be32(IMG_SEGS_EA + 0x08, IMG_CODE_LEN);
    wr_be32(IMG_SEGS_EA + 0x10, IMG_CODE_EA);
    wr_be32(IMG_SEGS_EA + 0x14, IMG_CODE_EA);

    /* seg 1: FILL, the shape an ELF's memsz>filesz tail takes. */
    wr_be32(IMG_SEGS_EA + 0x18 + 0x00, 2);
    wr_be32(IMG_SEGS_EA + 0x18 + 0x04, IMG_FILL_LS);
    wr_be32(IMG_SEGS_EA + 0x18 + 0x08, IMG_FILL_LEN);
    wr_be32(IMG_SEGS_EA + 0x18 + 0x10, IMG_FILL_BYTE);

    wr_be32(IMG_EA + 0x00, 0);            /* type = user image */
    wr_be32(IMG_EA + 0x04, IMG_ENTRY);
    wr_be32(IMG_EA + 0x08, IMG_SEGS_EA);
    wr_be32(IMG_EA + 0x0C, 2);            /* nsegs */
}

/* ---------------------------------------------------------------------------
 * What the lifted code saw, one slot per SPU thread
 * -----------------------------------------------------------------------*/
typedef struct {
    int            ran;
    pthread_t      host_thread;
    const uint8_t* ls;
    uint32_t       entry_pc;
    int            image_id;
    uint64_t       arg[4];
    int            copy_ok;
    int            fill_ok;
    int            concurrent;   /* both threads were inside at the same time */
} observation;

static observation g_obs[2];

/* Barrier across the two SPU threads. A run that serialized them cannot get
 * both here, so this is what turns "they ran on their own host threads" into
 * something that fails rather than something taken on trust. It times out so
 * that a failure is a failing test and not a hung one. */
static pthread_mutex_t g_bar_mu  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_bar_cv  = PTHREAD_COND_INITIALIZER;
static int             g_bar_n   = 0;

static int barrier_wait_two(void)
{
    struct timespec deadline;
    int ok = 1;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 10;

    pthread_mutex_lock(&g_bar_mu);
    if (++g_bar_n >= 2) {
        pthread_cond_broadcast(&g_bar_cv);
    } else {
        while (g_bar_n < 2) {
            if (pthread_cond_timedwait(&g_bar_cv, &g_bar_mu, &deadline) != 0) {
                ok = 0;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_bar_mu);
    return ok;
}

/* ---------------------------------------------------------------------------
 * The lifted SPU function
 *
 * Hand-written in the shape the lifter emits: it takes the context, works on
 * ctx->ls and ctx->gpr, and finishes by writing its exit status to the
 * outbound mailbox and executing stop 0x102, which is the SPU side of
 * sys_spu_thread_exit. The status is written straight into the channel rather
 * than through spu_wrch, to keep this about the start and exit path and not
 * about mailbox delivery to a PPU that is not here.
 * -----------------------------------------------------------------------*/
static void tiny_spu_entry(spu_context* ctx)
{
    int slot = (ctx->spu_id & 1u) ? 1 : 0;
    observation* o = &g_obs[slot];

    o->ran         = 1;
    o->host_thread = pthread_self();
    o->ls          = ctx->ls;
    o->entry_pc    = ctx->pc;
    o->image_id    = ctx->image_id;
    for (int a = 0; a < 4; a++)
        o->arg[a] = ((uint64_t)ctx->gpr[3 + a]._u32[0] << 32) |
                    (uint64_t)ctx->gpr[3 + a]._u32[1];

    o->copy_ok = 1;
    for (uint32_t i = 0; i < IMG_CODE_LEN; i++)
        if (ctx->ls[IMG_CODE_LS + i] != (uint8_t)(0x40u + i)) o->copy_ok = 0;
    o->fill_ok = (ctx->ls[IMG_ENTRY] == IMG_FILL_BYTE) &&
                 (ctx->ls[IMG_FILL_LS] == IMG_FILL_BYTE);

    o->concurrent = barrier_wait_two();

    /* Something for the PPU to read back out of local store afterwards. */
    ctx->ls[RESULT_LS] = (uint8_t)(RESULT_BYTE + slot);

    /* sys_spu_thread_exit: status to SPU_WrOutMbox, then stop 0x102. Thread 1
     * exits with a failure so the group's join has a worst status to find. */
    spu_channel_write(&ctx->ch_out_mbox, slot ? (uint32_t)(int32_t)-7 : 0u);
    ctx->stop_code = 0x102u;
    ctx->status    = SPU_STATUS_STOPPED_BY_STOP;
    spu_stop(ctx);
}

/* ---------------------------------------------------------------------------
 * Driving the syscalls
 * -----------------------------------------------------------------------*/
static ppu_context g_ppu;

static int64_t call(int64_t (*handler)(ppu_context*),
                    uint64_t a3, uint64_t a4, uint64_t a5,
                    uint64_t a6, uint64_t a7, uint64_t a8)
{
    memset(&g_ppu, 0, sizeof(g_ppu));
    g_ppu.gpr[3] = a3; g_ppu.gpr[4] = a4; g_ppu.gpr[5] = a5;
    g_ppu.gpr[6] = a6; g_ppu.gpr[7] = a7; g_ppu.gpr[8] = a8;
    return handler(&g_ppu);
}

static uint32_t rd_be32(uint32_t ea)
{
    const uint8_t* p = g_guest_mem + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* ---------------------------------------------------------------------------
 * The SMC micro-interpreter steps past branch hints.
 *
 * Runtime-generated stub code -- the WWS/SPURS jobmanager writes it into local
 * store and calls it, so it is never lifted -- reaches spu_indirect_branch,
 * which finds no lifted function for it and micro-interprets the live bytes
 * until a branch lands back in lifted code. hbra and hbrr are RI18-form branch
 * hints, 7-bit opcodes 0x08 and 0x09 (the same op7 group as ila), and no-ops
 * for execution. A decoder that reads the wrong opcode field treats the hint
 * as UNKNOWN, bails, and drops the SPU into branch-to-0: Yakuza: Dead Souls'
 * EDGE geometry task carried an hbrr and died exactly there. The snippet puts
 * a real hbrr (0x12033296, the word from that task) and an hbra between two
 * il's and an indirect branch -- if either hint is not stepped over, the il
 * after it never runs and the branch is never reached.
 * -----------------------------------------------------------------------*/
static void ls_put_be32(uint8_t* p, uint32_t w)
{ p[0]=(uint8_t)(w>>24); p[1]=(uint8_t)(w>>16); p[2]=(uint8_t)(w>>8); p[3]=(uint8_t)w; }

static void test_smc_branch_hints(void)
{
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    if (!ctx) { check(0, "calloc spu_context for the SMC hint test"); return; }
    spu_context_init(ctx, 0);

    const uint32_t PC = 0x400, TARGET = 0x2000;
    ls_put_be32(&ctx->ls[PC + 0x00], 0x40800282u);   /* il   $2, 5            */
    ls_put_be32(&ctx->ls[PC + 0x04], 0x10000000u);   /* hbra  (op7 0x08) hint */
    ls_put_be32(&ctx->ls[PC + 0x08], 0x12033296u);   /* hbrr  (op7 0x09) hint */
    ls_put_be32(&ctx->ls[PC + 0x0C], 0x40800383u);   /* il   $3, 7            */
    ls_put_be32(&ctx->ls[PC + 0x10], 0x35000380u);   /* bi   $7               */

    ctx->gpr[7]._u32[0] = TARGET;                    /* the bi's target        */
    ctx->pc = PC;                                    /* 0x400: nothing lifted here */
    ctx->image_id = 0;
    ctx->resident_ovl = 0;
    ctx->policy_mode = 0;

    spu_indirect_branch(ctx);

    check(ctx->gpr[2]._u32[0] == 5, "the il before the hints ran");
    check(ctx->gpr[3]._u32[0] == 7,
          "the il after the hints ran (a hint left as UNKNOWN is the branch-to-0 bug)");
    check(ctx->pc == TARGET, "the indirect branch past the hints was reached");

    g_spu_trampoline_fn = 0;                          /* don't leak the pending transfer */
    free(ctx);
}

/* Yakuza's generated job stub at LS 0x4C10 toggles a word using xori.
 * Exercise the observed aliasing form and a negative immediate in every lane. */
static void test_smc_xori(void)
{
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    if (!ctx) { check(0, "allocate SMC XOR context"); return; }
    spu_context_init(ctx, 0);
    const uint32_t pc = 0x500, target = 0x2000;
    ls_put_be32(&ctx->ls[pc], 0x44012850u); /* xori $80,$80,4 */
    ls_put_be32(&ctx->ls[pc + 4], 0x44FFE851u); /* xori $81,$80,-1 */
    ls_put_be32(&ctx->ls[pc + 8], 0x35000380u); /* bi $7 */
    const uint32_t lanes[4] = {0x4400A850u, 0x32000080u, 0, 0xFFFFFFFFu};
    for (int i = 0; i < 4; ++i) ctx->gpr[80]._u32[i] = lanes[i];
    ctx->gpr[7]._u32[0] = target;
    ctx->pc = pc;
    spu_indirect_branch(ctx);
    int alias_ok = 1, signed_ok = 1;
    for (int i = 0; i < 4; ++i) {
        alias_ok &= ctx->gpr[80]._u32[i] == (lanes[i] ^ 4u);
        signed_ok &= ctx->gpr[81]._u32[i] == ((lanes[i] ^ 4u) ^ 0xFFFFFFFFu);
    }
    check(alias_ok, "SMC xori applies to all lanes with rt == ra");
    check(signed_ok, "SMC xori sign-extends its 10-bit immediate");
    check(ctx->pc == target, "SMC xori continues to the following branch");
    g_spu_trampoline_fn = 0;
    free(ctx);
}

static void code_a(spu_context* ctx) { ctx->gpr[3]._u32[0] = 81; }
static void code_b(spu_context* ctx) { ctx->gpr[3]._u32[0] = 82; }
static void code_stale(spu_context* ctx) { ctx->gpr[3]._u32[0] = 83; }
static void test_resident_code_regions(void)
{
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    if (!ctx) { check(0, "allocate streamed code context"); return; }
    spu_context_init(ctx, 0);
    const uint32_t a = 0x11000, b = 0x12000;
    spu_begin_image(81); spu_register_function(a, code_a);
    spu_begin_image(82); spu_register_function(b, code_b);
    spu_begin_image(83); spu_register_function(a, code_stale);
    spu_register_function(b, code_stale);
    spu_begin_image(0);
    spu_overlay_register_region(0x500000, 0x400, 81);
    spu_overlay_register_region(0x600000, 0x400, 82);
    ctx->image_id = 83;
    spu_overlay_note_get(ctx, 0x500000, ctx->ls + a, 0x100);
    spu_overlay_note_get(ctx, 0x600000, ctx->ls + b, 0x100);
    spu_overlay_note_get(ctx, 0x500100, ctx->ls + a + 0x100, 0x100);
    ctx->pc = a; spu_indirect_branch(ctx);
    check(ctx->gpr[3]._u32[0] == 81, "first streamed job survives second job and continuation DMA");
    ctx->pc = b; spu_indirect_branch(ctx);
    check(ctx->gpr[3]._u32[0] == 82, "second streamed job overrides stale base-image code");
    spu_overlay_note_get(ctx, 0x700000, ctx->ls + a, 0x100);
    ctx->pc = a; spu_indirect_branch(ctx);
    check(ctx->gpr[3]._u32[0] != 81, "overwriting a code buffer invalidates its old image");
    ctx->pc = b; spu_indirect_branch(ctx);
    check(ctx->gpr[3]._u32[0] == 82, "overwriting one buffer preserves the other");
    g_spu_trampoline_fn = 0;
    free(ctx);
}

/* A real taskset also stores 0xA70 in its API table. That address alone
 * cannot distinguish Sony's resident scheduler from a synthetic HLE task. */
static void test_lle_taskset_syscall(void)
{
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    if (!ctx) { check(0, "allocate taskset context"); return; }
    spu_context_init(ctx, 0);
    spu_begin_image(84); spu_register_function(0xA70, code_a);
    spu_begin_image(0);
    ctx->image_id = 16;
    ctx->resident_ovl = 84;
    ctx->ls[0x27C6] = 0x0A; ctx->ls[0x27C7] = 0x70;
    ctx->gpr[3]._u32[0] = 3;
    ctx->pc = 0xA70; spu_indirect_branch(ctx);
    check(ctx->gpr[3]._u32[0] == 81, "LLE taskset syscall executes resident policy code");
    ctx->policy_mode = 1;
    ctx->gpr[3]._u32[0] = 3;
    ctx->pc = 0xA70; spu_indirect_branch(ctx);
    check(ctx->gpr[3]._u32[0] == 0, "synthetic HLE taskset retains syscall emulation");
    g_spu_trampoline_fn = 0;
    free(ctx);
}

extern void spu_taskset_register_task_elf(uint32_t, int, int);
static void test_taskset_resume_identity(void)
{
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    if (!ctx) { check(0, "allocate task resume context"); return; }
    spu_context_init(ctx, 0);
    spu_begin_image(85); spu_register_function(0x7BD4, code_a);
    spu_begin_image(86); spu_register_function(0x7BD4, code_b);
    spu_begin_image(84); spu_register_function(0xA70, code_stale);
    spu_begin_image(0);
    spu_taskset_register_task_elf(0x500000, 85, 84);
    spu_taskset_register_task_elf(0x600000, 86, 84);
    ctx->image_id = 16; ctx->resident_ovl = 84;
    ctx->ls[0x2795] = 0x50; ctx->ls[0x2797] = 3; /* ELF flag bits */
    ctx->resident_task = 85;
    ctx->pc = 0xA70; spu_indirect_branch(ctx);
    ctx->pc = 0x7BD4; spu_indirect_branch(ctx);
    check(ctx->gpr[3]._u32[0] == 81, "scheduler return resolves the original task's internal PC");
    ctx->pc = 0xA70; spu_indirect_branch(ctx);
    ctx->ls[0x2795] = 0x60;
    ctx->pc = 0x7BD4; spu_indirect_branch(ctx);
    check(ctx->gpr[3]._u32[0] == 82, "resuming another ELF switches ownership of the same PC");
    g_spu_trampoline_fn = 0;
    free(ctx);
}

extern void spu_register_stack_reset_entry(uint32_t, int);
extern void spu_halt(spu_context*);
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
static int stack_reset_depth, stale_caller_ran;
static void reset_target(spu_context* ctx)
{
    stack_reset_depth = (int)ctx->host_depth;
    spu_halt(ctx);
}
static void nested_reset(spu_context* ctx)
{
    ctx->host_depth += 4;
    ctx->pc = 0x838;
    spu_indirect_branch(ctx);
    stale_caller_ran = 1;
}
static void direct_reset(spu_context* ctx)
{
    ctx->host_depth = 3;
    ctx->pc = 0x838;
    g_spu_trampoline_fn = reset_target;
    SPU_DRAIN(ctx);
    stale_caller_ran = 1;
}
static void test_guest_stack_reset(void)
{
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    if (!ctx) { check(0, "allocate stack reset context"); return; }
    spu_context_init(ctx, 0);
    spu_begin_image(87); spu_register_function(0x838, reset_target);
    spu_begin_image(0);
    spu_register_stack_reset_entry(0x838, 87);
    ctx->image_id = 87;
    stack_reset_depth = -1; stale_caller_ran = 0;
    spu_run_with_halt(nested_reset, ctx);
    check(stack_reset_depth == 0, "one-way kernel entry discards nested host call frames");
    check(!stale_caller_ran, "exited workload cannot resume its obsolete caller");
    stack_reset_depth = -1; stale_caller_ran = 0;
    spu_run_with_halt(direct_reset, ctx);
    check(stack_reset_depth == 0 && !stale_caller_ran,
          "direct trampoline transfers honor registered stack resets");
    free(ctx);
}

static void nested_task_resume(spu_context* ctx)
{
    ctx->host_depth = 5;
    ctx->pc = 0x7BD4;
    spu_indirect_branch(ctx);
    stale_caller_ran = 1;
}
static void test_taskset_resume_stack(void)
{
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    if (!ctx) { check(0, "allocate task resume stack"); return; }
    spu_context_init(ctx, 0);
    spu_begin_image(88); spu_register_function(0x7BD4, reset_target);
    spu_begin_image(0);
    spu_taskset_register_task_elf(0x700000, 88, 84);
    ctx->image_id = 16; ctx->resident_ovl = 84;
    ctx->ls[0x2795] = 0x70;
    stack_reset_depth = -1; stale_caller_ran = 0;
    spu_run_with_halt(nested_task_resume, ctx);
    check(stack_reset_depth == 0, "restored task runs without the scheduler's host frames");
    check(!stale_caller_ran, "task resume discards the old scheduler continuation");
    free(ctx);
}

static int duplicate_continuation;
static void caller_continuation(spu_context* ctx)
{
    ++duplicate_continuation;
    ctx->gpr[1]._u32[0] += 16;
}
static void test_alternate_link_return(void)
{
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    if (!ctx) { check(0, "allocate alternate return context"); return; }
    spu_context_init(ctx, 0);
    spu_begin_image(89); spu_register_function(0x900, caller_continuation);
    spu_begin_image(0);
    ctx->image_id = 89; ctx->host_depth = 1;
    ctx->gpr[0]._u32[0] = 0x888; /* callee used r0 for a nested call */
    ctx->gpr[1]._u32[0] = 0x2c00;
    ctx->pc = 0x900; /* bi r7, with r7 holding the caller's saved link */
    g_spu_trampoline_fn = spu_indirect_branch;
    duplicate_continuation = 0;
    spu_drain_call(ctx, 0x900);
    check(!duplicate_continuation && ctx->gpr[1]._u32[0] == 0x2c00,
          "alternate-register return does not execute the caller inside the callee");
    check(!g_spu_trampoline_fn, "alternate return leaves no pending continuation");
    ctx->policy_mode = 1;
    ctx->ls[0x27C6] = 0x0a; ctx->ls[0x27C7] = 0x70;
    ctx->gpr[0]._u32[0] = 0x900; ctx->gpr[3]._u32[0] = 3;
    ctx->pc = 0xa70; spu_indirect_branch(ctx); spu_drain_call(ctx, 0x900);
    check(ctx->pc == 0x900 && ctx->gpr[3]._u32[0] == 0,
          "synthetic HLE calls publish their architectural return PC");
    free(ctx);
}

static uint32_t captured_queue;
static uint64_t captured_event[4];
static int push_result;
static uint32_t captured_flag;
static uint64_t captured_flag_bits;
int64_t sys_event_flag_set(ppu_context* ctx)
{
    captured_flag = (uint32_t)ctx->gpr[3];
    captured_flag_bits = ctx->gpr[4];
    return captured_flag == 44 ? CELL_OK : (int64_t)(int32_t)CELL_ESRCH;
}

static void test_user_event_ports(uint32_t gid, uint32_t tid)
{
    check(call(sys_spu_thread_group_connect_event_all_threads_handler,
        gid, 77, 0xFFFFFFFFFFFF0000ull, OUT_EA, 0, 0) == 0,
        "allocate a requested SPU user-event port");
    check(vm_base[OUT_EA] == 16, "return the allocated port through the byte out-param");
    check(call(sys_spu_thread_group_connect_event_all_threads_handler,
        gid, 78, 1ull << 16, OUT_EA, 0, 0) == (int32_t)CELL_EISCONN,
        "refuse an already connected requested port");
    check(call(sys_spu_thread_group_connect_event_all_threads_handler,
        gid, 78, 3ull << 16, OUT_EA, 0, 0) == 0 && vm_base[OUT_EA] == 17,
        "allocate another port without replacing the first");
    check(call(sys_spu_thread_group_connect_event_all_threads_handler,
        gid, 78, 0, OUT_EA, 0, 0) == (int32_t)CELL_EINVAL,
        "reject an empty port mask");
    spu_context* spu = (spu_context*)calloc(1, sizeof *spu);
    if (!spu) { check(0, "allocate event context"); return; }
    spu_context_init(spu, 0);
    spu->spu_group_id = gid;
    spu->spu_id = tid;
    extern int (*g_spu_user_event_hook)(spu_context*, uint32_t);
    g_spu_user_event_hook = spu_deliver_user_event;
    captured_queue = 0;
    spu_wrch(spu, SPU_WrOutMbox, spu_splat_u32(0x12345678));
    check(captured_queue == 0, "ordinary mailbox data does not emit a PPU event");
    spu_wrch(spu, SPU_WrOutIntrMbox, spu_splat_u32(0x1000ABCD));
    check(captured_queue == 77 && captured_event[0] == 0xFFFFFFFF53505501ull &&
          captured_event[1] == tid && captured_event[2] == 0x100000ABCDull &&
          captured_event[3] == 0x12345678,
          "send_event routes the port, source and payload correctly");
    check(spu->ch_out_mbox.count == 0 && spu->ch_in_mbox.count == 1 &&
          spu_channel_read(&spu->ch_in_mbox) == 0, "send_event consumes data and acknowledges success");
    push_result = -1;
    spu_wrch(spu, SPU_WrOutMbox, spu_splat_u32(1));
    spu_wrch(spu, SPU_WrOutIntrMbox, spu_splat_u32(0x10000000));
    check(spu_channel_read(&spu->ch_in_mbox) == CELL_EBUSY,
          "send_event reports a full queue to the SPU");
    push_result = 0;
    spu_wrch(spu, SPU_WrOutMbox, spu_splat_u32(2));
    spu_wrch(spu, SPU_WrOutIntrMbox, spu_splat_u32(0x51000000));
    check(captured_queue == 78 && spu->ch_in_mbox.count == 0,
          "throw_event uses its own port and produces no acknowledgement");
    spu_wrch(spu, SPU_WrOutMbox, spu_splat_u32(3));
    spu_wrch(spu, SPU_WrOutIntrMbox, spu_splat_u32(0x12000000));
    check(spu_channel_read(&spu->ch_in_mbox) == CELL_ENOTCONN,
          "send_event reports an unbound port");
    captured_queue = 0;
    spu_wrch(spu, SPU_WrOutMbox, spu_splat_u32(44));
    spu_wrch(spu, SPU_WrOutIntrMbox, spu_splat_u32(0xC000003F));
    check(captured_flag == 44 && captured_flag_bits == (1ull << 63) &&
          captured_queue == 0 && spu->ch_in_mbox.count == 0,
          "impatient flag request sets its bit without a queue event or ack");
    spu_wrch(spu, SPU_WrOutMbox, spu_splat_u32(44));
    spu_wrch(spu, SPU_WrOutIntrMbox, spu_splat_u32(0x80000000));
    check(captured_flag_bits == 1 && spu_channel_read(&spu->ch_in_mbox) == CELL_OK,
          "strict flag request sets bit zero and acknowledges success");
    spu_wrch(spu, SPU_WrOutMbox, spu_splat_u32(45));
    spu_wrch(spu, SPU_WrOutIntrMbox, spu_splat_u32(0x80000002));
    check(spu_channel_read(&spu->ch_in_mbox) == CELL_ESRCH,
          "strict flag request reports an unknown flag");
    spu_wrch(spu, SPU_WrOutMbox, spu_splat_u32(44));
    spu_wrch(spu, SPU_WrOutIntrMbox, spu_splat_u32(0x80000040));
    check(spu_channel_read(&spu->ch_in_mbox) == CELL_EINVAL,
          "flag request rejects a bit outside the 64-bit pattern");
    g_spu_user_event_hook = NULL;
    free(spu);
}

/* The registry holds one entry per lifted function across EVERY image, and a
 * whole game's SPU workload is far more than one image: Yakuza registers ~170k.
 * Images register in dependency order, so a too-small cap silently dropped the
 * LAST ones -- the SPURS job-chain policy, the job binaries and the Edge
 * geometry task -- and every indirect branch into them fell through to a
 * branch-to-0 that read as a lifter bug. Register well past the old 65536 cap
 * and confirm the entries above it survive and are found, so a too-small
 * registry can never return as that silent, mislabelled failure.
 *
 * Runs last: its filler entries use their own image id and a key range clear of
 * the group test's, so they cannot shadow anything the checks above relied on. */
static void test_registry_capacity(void)
{
    const uint32_t OLD_CAP = 65536u;             /* the cap this fix raised */
    const uint32_t N       = OLD_CAP + 4096u;    /* comfortably past it */
    const uint32_t BASE    = 0x00080000u;        /* unique keys, clear of LS and IMG_ENTRY */
    const int      IMG     = 77;                 /* its own image id */

    spu_begin_image(IMG);
    for (uint32_t i = 0; i < N; i++)
        spu_register_function(BASE + i * 4u, tiny_spu_entry);

    check(spu_lookup(BASE + 100u * 4u, IMG) == tiny_spu_entry,
          "an early registry entry is found");
    check(spu_lookup(BASE + OLD_CAP * 4u, IMG) == tiny_spu_entry,
          "an entry at the old 65536 cap is retained (it used to be dropped)");
    check(spu_lookup(BASE + (N - 1u) * 4u, IMG) == tiny_spu_entry,
          "the last entry, well past the old cap, is retained");
}

/* Short list elements occupy separate quadwords in LS. The effective
 * address supplies the byte offset within each quadword. Chroma rows in
 * movie decoding use eight-byte elements, making packed advancement corrupt
 * every other block even though ordinary sixteen-byte transfers work. */
static void submit_short_dma_list(spu_context* ctx, uint32_t cmd)
{
    spu_wrch(ctx, MFC_LSA, spu_pref_u32(0x207)); /* low bits ignored for lists */
    spu_wrch(ctx, MFC_EAH, spu_zero());
    spu_wrch(ctx, MFC_EAL, spu_pref_u32(0x100));
    spu_wrch(ctx, MFC_Size, spu_pref_u32(24));
    spu_wrch(ctx, MFC_TagID, spu_pref_u32(2));
    spu_wrch(ctx, MFC_Cmd, spu_pref_u32(cmd));
}

static void test_short_dma_lists(void)
{
    static spu_context ctx;
    spu_context_init(&ctx, 0);
    /* This arena was already committed with mprotect above. */
    extern uint8_t g_vm_page_bitmap[];
    g_vm_page_bitmap[1] |= 1;
    const uint32_t ea[] = {0x80000, 0x80028, 0x80040};
    const uint32_t ls[] = {0x200, 0x218, 0x220};
    memset(ctx.ls + 0x200, 0xCC, 48);
    for (unsigned i = 0; i < 3; ++i) {
        spu_ls_write32(&ctx, 0x100 + i * 8, 8);
        spu_ls_write32(&ctx, 0x104 + i * 8, ea[i]);
        memset(ctx.ls + ls[i], 0x31 + i, 8);
        memset(vm_base + ea[i], 0, 8);
    }
    submit_short_dma_list(&ctx, MFC_PUTL_CMD);
    for (unsigned i = 0; i < 3; ++i)
        check(vm_base[ea[i]] == 0x31 + i && vm_base[ea[i] + 7] == 0x31 + i,
              "short PUT list reads the correct LS quadword and byte offset");

    /* The second GET stalls; resuming must retain the rounded cursor. */
    spu_ls_write32(&ctx, 0x108, 0x80000008u);
    memset(ctx.ls + 0x200, 0xCC, 48);
    submit_short_dma_list(&ctx, MFC_GETL_CMD);
    check(ctx.list_stall_dest_lsa[2] == 0x220 && ctx.ls[0x220] == 0xCC,
          "short GET list parks after its second quadword");
    spu_wrch(&ctx, MFC_WrListStallAck, spu_pref_u32(2));
    for (unsigned i = 0; i < 3; ++i)
        check(ctx.ls[ls[i]] == 0x31 + i && ctx.ls[ls[i] + 7] == 0x31 + i,
              "short GET list preserves alignment across stall acknowledgement");
    check(ctx.ls[0x208] == 0xCC && ctx.ls[0x210] == 0xCC && ctx.ls[0x228] == 0xCC,
          "short GET list leaves quadword padding untouched");
}

/* A lifted return through a NON-r0 link register. Sony's SPU compiler links
 * leaf/helper calls through r4/r5/r6/r8/r78 and returns with `bi $rN`; the
 * lifter lowers that to SPU_RET_REG(ctx, N). SPU_RET publishes r0 instead,
 * which is not where the guest is going, and spu_drain_call -- which stops a
 * translated call at its explicit return PC -- then sees pc != return_pc and
 * restarts dispatch from r0's stale value. Observed on LittleBigPlanet's
 * wwsjob policy module: every `brsl $r4, helper` ... `bi $r4` tore the
 * module out of its host frames two DMAs into its first run. */
static void callee_returns_via_r7(spu_context* ctx) { SPU_RET_REG(ctx, 7); }
static void callee_returns_via_r0(spu_context* ctx) { SPU_RET(ctx); }
static void test_nonzero_link_return(void)
{
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    if (!ctx) { check(0, "allocate non-r0 return context"); return; }
    spu_begin_image(90); spu_begin_image(0);
    ctx->image_id = 90; ctx->host_depth = 1;
    ctx->gpr[7]._u32[0] = 0x900;  /* the link the caller planted with brsl $r7 */
    ctx->gpr[0]._u32[0] = 0x888;  /* r0 holds an unrelated nested-call link */
    ctx->pc = 0x400;
    g_spu_trampoline_fn = 0;
    callee_returns_via_r7(ctx);
    spu_drain_call(ctx, 0x900);
    check(ctx->pc == 0x900, "SPU_RET_REG publishes the link register it returns through");
    check(!g_spu_trampoline_fn, "non-r0 link return leaves no pending continuation");
    check(ctx->status != SPU_STATUS_STOPPED_BY_HALT,
          "spu_drain_call accepts the non-r0 return at its return address");

    /* Negative control: the old lowering. r0 is not the return register, so
     * the drain's return-address check cannot match and it restarts dispatch. */
    ctx->host_depth = 1; ctx->status = 0; ctx->pc = 0x400;
    g_spu_trampoline_fn = 0;
    callee_returns_via_r0(ctx);
    spu_drain_call(ctx, 0x900);
    check(ctx->pc == 0x888, "SPU_RET publishes r0 (the defect the macro fixes)");
    check(ctx->status == SPU_STATUS_STOPPED_BY_HALT,
          "spu_drain_call restarts dispatch when a return publishes the wrong register");
    free(ctx);
}

int main(void)
{
    printf("SPU lifted thread-group start\n");

    /* Runtime-generated branch hints, before the registry has anything in it
     * so the micro-interpreter path is the one that runs. */
    test_smc_branch_hints();
    test_smc_xori();
    test_resident_code_regions();
    test_lle_taskset_syscall();
    test_taskset_resume_identity();
    test_guest_stack_reset();
    test_taskset_resume_stack();
    test_alternate_link_return();
    test_nonzero_link_return();

    signal(SIGSEGV, guard_fault);
    signal(SIGBUS,  guard_fault);

    /* Arena and guard in one mapping, so nothing can be placed between them,
     * then the arena alone made readable. */
    g_guest_mem = (uint8_t*)mmap(NULL, GUEST_SIZE + GUEST_GUARD, PROT_NONE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                 -1, 0);
    if (g_guest_mem == MAP_FAILED) {
        printf("  FAIL: could not reserve %u bytes of guest memory\n", GUEST_SIZE);
        return 1;
    }
    if (mprotect(g_guest_mem, GUEST_SIZE, PROT_READ | PROT_WRITE) != 0) {
        printf("  FAIL: could not map the %u byte guest arena\n", GUEST_SIZE);
        return 1;
    }
    vm_base = g_guest_mem;
    test_short_dma_lists();

    /* Ask for the group-start diagnostic that reads one title's SPURS instance
     * at a fixed 0x40009D00, a thousand times past the end of this arena, so
     * the start path below runs with it armed. Its bounds check against
     * ppu_vm_size is then the only thing between it and the guard region. */
    setenv("YDKJ_INSTDUMP", "1", 1);

    build_guest_image();

    /* The title's registration: lifted code for this image, under its own id. */
    spu_begin_image(IMG_IMAGE_ID);
    spu_register_function(IMG_ENTRY, tiny_spu_entry);
    spu_begin_image(0);

    /* Two threads, each with its own argument block. Distinct values, so a
     * thread reading the other's is visible rather than plausible. */
    for (int a = 0; a < 4; a++) {
        wr_be64(ARGS0_EA + (uint32_t)a * 8, 0x1000000000ull + (uint64_t)a);
        wr_be64(ARGS1_EA + (uint32_t)a * 8, 0x2000000000ull + (uint64_t)a);
    }

    /* --- create ------------------------------------------------------- */
    CHECK(call(sys_spu_thread_group_create_handler, OUT_EA, 2, 250, 0, 0, 0) == 0);
    uint32_t gid = rd_be32(OUT_EA);
    CHECK(gid != 0);

    /* --- initialize --------------------------------------------------- */
    CHECK(call(sys_spu_thread_initialize_handler,
               OUT_EA, gid, 0, IMG_EA, 0, ARGS0_EA) == 0);
    uint32_t tid0 = rd_be32(OUT_EA);
    CHECK(call(sys_spu_thread_initialize_handler,
               OUT_EA, gid, 1, IMG_EA, 0, ARGS1_EA) == 0);
    uint32_t tid1 = rd_be32(OUT_EA);
    CHECK(tid0 != tid1);
    /* The entry point comes off the image, so the registry can be asked. */
    CHECK(spu_find_thread(tid0)->entry_point == IMG_ENTRY);
    /* Slot selection below keys on the low bit of the tid; if lv2 ever hands
     * out two ids of the same parity the two threads would share a slot. */
    CHECK(((tid0 ^ tid1) & 1u) == 1u);

    test_user_event_ports(gid, tid0);

    /* --- start and join ----------------------------------------------- */
    CHECK(call(sys_spu_thread_group_start_handler, gid, 0, 0, 0, 0, 0) == 0);
    CHECK(call(sys_spu_thread_group_join_handler, gid, OUT_EA, OUT_EA + 4,
               0, 0, 0) == 0);
    uint32_t cause  = rd_be32(OUT_EA);
    int32_t  status = (int32_t)rd_be32(OUT_EA + 4);

    /* --- what the lifted code saw -------------------------------------- */
    observation* a = &g_obs[tid0 & 1u];
    observation* b = &g_obs[tid1 & 1u];

    CHECK(a->ran);                              /* it ran at all */
    CHECK(b->ran);
    if (!a->ran || !b->ran) {
        printf("SPU lifted thread-group start: %d passed, %d failed\n",
               g_pass, g_fail);
        return 1;
    }

    /* Its own host thread, concurrently with the other, and neither of them
     * the thread that called group_start. */
    CHECK(!pthread_equal(a->host_thread, b->host_thread));
    CHECK(!pthread_equal(a->host_thread, pthread_self()));
    CHECK(!pthread_equal(b->host_thread, pthread_self()));
    CHECK(a->concurrent);
    CHECK(b->concurrent);

    /* Its own local store, and the one lv2 hands out for this thread. */
    CHECK(a->ls != b->ls);
    CHECK(a->ls == spu_thread_get_local_store(tid0));
    CHECK(b->ls == spu_thread_get_local_store(tid1));

    /* The image really landed in it: COPY copied, FILL filled. */
    CHECK(a->copy_ok);
    CHECK(b->copy_ok);
    CHECK(a->fill_ok);
    CHECK(b->fill_ok);

    /* Entry and image: pc at the image's entry point, dispatching in the
     * image that registered it rather than in the id-0 wildcard. */
    CHECK(a->entry_pc == IMG_ENTRY);
    CHECK(b->entry_pc == IMG_ENTRY);
    CHECK(a->image_id == IMG_IMAGE_ID);
    CHECK(b->image_id == IMG_IMAGE_ID);

    /* Each thread's OWN arguments, in the preferred doubleword of r3..r6. */
    for (int i = 0; i < 4; i++) {
        CHECK(a->arg[i] == 0x1000000000ull + (uint64_t)i);
        CHECK(b->arg[i] == 0x2000000000ull + (uint64_t)i);
    }

    /* The status each SPU wrote on its way out, and the group's. */
    CHECK(spu_find_thread(tid0)->exit_status == 0);
    CHECK(spu_find_thread(tid1)->exit_status == -7);
    CHECK(cause == SPU_GROUP_CAUSE_ALL_THREADS_EXIT);
    CHECK(status == -7);

    /* And the PPU can read back what the SPU left in local store, which it
     * can only do if the two views of it are the same 256 KB. The syscall
     * writes its result as a big-endian u64 at a guest address. */
    CHECK(call(sys_spu_thread_read_ls_handler, tid0, RESULT_LS,
               OUT_EA + 8, 1, 0, 0) == 0);
    CHECK(g_guest_mem[OUT_EA + 15] == (uint8_t)(RESULT_BYTE + (int)(tid0 & 1u)));

    /* An image with no lifted code must still take the old path. */
    CHECK(!spu_lifted_thread_available(IMG_ENTRY + 4));

    /* The registry is sized for a whole game's SPU workload. Last, so its
     * filler registrations cannot affect the lookups above. */
    test_registry_capacity();

    printf("SPU lifted thread-group start: %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * The runtime beyond the SPU and lv2 layers, stubbed.
 *
 * These are the symbols lv2_register.c and spu_channels.c reach for outside
 * their own subsystems: the other syscall families, the PPU's backtrace and
 * watch helpers, and the title-specific SPURS hooks. None of them is on the
 * path this test drives.
 * -----------------------------------------------------------------------*/
void sys_ppu_thread_init(lv2_syscall_table* t) { (void)t; }
void sys_mutex_init(lv2_syscall_table* t)      { (void)t; }
void sys_cond_init(lv2_syscall_table* t)       { (void)t; }
void sys_semaphore_init(lv2_syscall_table* t)  { (void)t; }
void sys_rwlock_init(lv2_syscall_table* t)     { (void)t; }
void sys_event_init(lv2_syscall_table* t)      { (void)t; }
void sys_timer_init(lv2_syscall_table* t)      { (void)t; }
void sys_memory_init(lv2_syscall_table* t)     { (void)t; }
void sys_vm_init(lv2_syscall_table* t)         { (void)t; }
void sys_fs_init(lv2_syscall_table* t)         { (void)t; }

int sys_event_queue_push_by_id(uint32_t q, uint64_t d0, uint64_t d1,
                               uint64_t d2, uint64_t d3)
{
    captured_queue = q;
    captured_event[0] = d0; captured_event[1] = d1;
    captured_event[2] = d2; captured_event[3] = d3;
    return push_result;
}
void sys_fs_translate_path(const char* ps3_path, char* host, int size)
{
    (void)ps3_path;
    if (host && size > 0) host[0] = 0;
}
void sys_process_exit(int32_t status)            { exit((int)status); }
int32_t sys_process_getpid(void)                 { return 1; }

void ppu_dump_guest_stack(ppu_context* c, const char* tag) { (void)c; (void)tag; }
void ppu_guard_page(uint32_t ea)                 { (void)ea; }
void ppu_guest_caller(char* out, size_t n)       { if (out && n) out[0] = 0; }
void ppu_guest_callstack(const char* tag)        { (void)tag; }
void ppu_log_host_chain(const char* tag)         { (void)tag; }
void ps3_hle_register_ctx(uint32_t nid, const char* name,
                          void (*fn)(ppu_context*))
{
    (void)nid; (void)name; (void)fn;
}
void ppu_resv_break_store(uint64_t ea)           { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
uint64_t ps3_ms_now(void)                        { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
int      g_barrier_sync_watch = 0;

/* Title-specific SPURS hooks the SPU half offers but nothing here installs. */
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint32_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

/* Upstream raw-SPU and RSX services are outside this group-start fixture. */
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t ps3_spu_image_source_ea(uint32_t ea) { return ea; }
void sys_raw_spu_init(lv2_syscall_table* t) { (void)t; }
void sys_rsx_init(lv2_syscall_table* t) { (void)t; }
void spu_raw_note_image(uint32_t ea, uint32_t src) { (void)ea; (void)src; }
int32_t spu_registry_fallback(uint32_t tid, uint32_t args, uint32_t size, void* user)
{ (void)tid; (void)args; (void)size; (void)user; return -1; }

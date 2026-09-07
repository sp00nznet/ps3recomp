/*
 * ps3recomp - the smoke title: a synthetic "lifted" PS3 program.
 *
 * runtime/ppu/ is the game-agnostic boot path every port is built on, and
 * until this existed nothing in the repository linked it, let alone ran it:
 * tools/check_ppu_scaffold.py compiles it against a stub header and stops
 * there, because a real title needs the lifter's output and a decrypted
 * EBOOT. So on POSIX the loader, the NID bridge, the lv2 syscalls, guest
 * threads, the frame clock and the RSX backend had never once run together.
 *
 * This file is what a lift produces, written by hand. It obeys the lifter's
 * ABI exactly -- ppu_recomp.h (generated from ppu_lifter.py's own
 * HEADER_PREAMBLE by tools/make_smoke_elf.py), one
 * `void func_<guest addr>(ppu_context*)` per guest function, a sentinel-
 * terminated function_table[] the runtime registers -- so everything it
 * touches is reached the way a real title reaches it:
 *
 *   - a line of output through the sys_tty_write syscall,
 *   - a firmware import through the NID bridge, as a lifted .lib.stub
 *     trampoline is: a function whose whole body is ps3_hle_call(nid, ctx),
 *   - a guest thread via sys_ppu_thread_create, joined via
 *     sys_ppu_thread_join,
 *   - cellGcm brought up through _cellGcmInitBody, then three frames cleared
 *     and flipped through the FIFO -- the same command words
 *     runtime/host/host_posix.c writes, but emitted from guest code into
 *     guest memory,
 *   - sys_process_exit with a chosen status.
 *
 * Every step prints a marker line, and the line comes out of the image's own
 * string table, so the text in the log is evidence that ppu_load_elf copied
 * the PT_LOAD data into guest RAM. The same steps are recorded as ids, and
 * smoke_verify() -- installed with atexit() just before the guest exits, so
 * it runs before any other handler -- checks the whole run and decides the
 * process's exit code. It is deliberately the guest that decides: the point
 * of the exercise is that the guest ran.
 */
#include "ppu_recomp.h"

/* The NV4097 method numbers the frame loop writes. Taken from the project's
 * own definitions rather than spelled out again: a wrong GPU method number is
 * the kind of mistake that produces a plausible, silent nothing. */
#include "rsx_commands.h"

/* GetCurrentThreadId: the host thread identity the assertion about the guest
 * thread is made of. On POSIX this is runtime/platform's shim, which is what
 * the rest of the scaffold uses. Through the include path, like rsx_commands.h
 * above: a lifted title's sources are built from wherever its port keeps them,
 * so a relative path into the toolkit only works while the file sits in this
 * directory. */
#include "win32_compat.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* Runtime entry points a lifted title reaches. ppu_lifter.py declares these in
 * its source preamble; the smoke title, having no generated source, declares
 * them here. */
extern void ps3_hle_call(unsigned int nid, ppu_context* ctx);
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

/* Drain pending cross-fragment trampolines after a call, exactly as the
 * lifter's DRAIN_TRAMPOLINE does. The smoke title emits no tail calls, so this
 * never has work to do -- it is here because a lifted call site always has it,
 * and the runtime's drain contract is part of what is being exercised. */
#define DRAIN_TRAMPOLINE(ctx) do { \
    while (g_trampoline_fn) { \
        void (*_tf)(void*) = g_trampoline_fn; \
        g_trampoline_fn = 0; \
        _tf((void*)(ctx)); \
    } \
} while (0)

/* Published by libs/system/sysPrxForUser.c before it calls the host exit(). An
 * atexit handler cannot see what exit() was called with, so this is the only
 * way to check that the status the guest chose is the status that arrived. */
extern int g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Frames the boot harness presented through the RSX backend, counted at the
 * flip boundary (runtime/ppu/tests/boot_main.cpp). */
extern unsigned ppu_boot_frames_presented(void);
/* Flip requests the guest made (libs/video/cellGcmSys.c). */
extern unsigned int cellGcm_flip_request_count(void);

/* ---------------------------------------------------------------------------
 * The guest address map.
 *
 * Mirrors tools/make_smoke_elf.py, which writes the image these addresses
 * describe and the ppu_recomp.h that declares the functions at them. The two
 * cannot silently disagree: the OPD in the image names a code address, and an
 * OPD naming an address no function registered makes ppu_run refuse to
 * dispatch and print why.
 * -----------------------------------------------------------------------*/
#define SMOKE_TEXT_BASE     0x00010000u
#define SMOKE_DATA_BASE     0x00020000u

#define SMOKE_OPD_ENTRY     (SMOKE_DATA_BASE + 0x0000u)   /* e_entry */
#define SMOKE_OPD_THREAD    (SMOKE_DATA_BASE + 0x0008u)   /* thread body */
#define SMOKE_OPD_VBLANK    (SMOKE_DATA_BASE + 0x0040u)   /* vblank handler */
#define SMOKE_STRTAB        (SMOKE_DATA_BASE + 0x0100u)
#define SMOKE_STR_STRIDE    0x60u
#define SMOKE_THREAD_NAME   (SMOKE_DATA_BASE + 0x0E00u)
#define SMOKE_CANARY_EA     (SMOKE_DATA_BASE + 0x0FF0u)
#define SMOKE_CANARY        0x50533352u                   /* 'PS3R' */
#define SMOKE_TLS_CANARY    0x544C5330u                   /* 'TLS0' */

/* Scratch for syscall out-parameters, in the image's BSS tail (memsz beyond
 * filesz), which is also what proves that tail is backed guest RAM. */
#define SMOKE_SC_TTYLEN     (SMOKE_DATA_BASE + 0x2000u)
#define SMOKE_SC_GCMCTX     (SMOKE_DATA_BASE + 0x2010u)
#define SMOKE_SC_TID        (SMOKE_DATA_BASE + 0x2020u)
#define SMOKE_SC_EXITSTAT   (SMOKE_DATA_BASE + 0x2030u)
#define SMOKE_SC_BSSPROBE   (SMOKE_DATA_BASE + 0x2040u)

/* The PS3 thread pointer sits 0x7000 above the thread's TLS block, which is
 * the offset ppu_run uses when it copies the PT_TLS template and sets r13. */
#define SMOKE_TLS_TP_BIAS   0x7000u

/* cellGcm bring-up, the same numbers runtime/host/host_posix.c uses. */
#define SMOKE_IO_ADDR       0x00100000u
#define SMOKE_IO_SIZE       0x00200000u
#define SMOKE_CMD_SIZE      0x00010000u
#define SMOKE_CLEAR_ARGB    0xFF204060u
#define SMOKE_FRAMES        3

/* lv2 syscall numbers (runtime/syscalls/lv2_syscall_table.h). Spelled out
 * because a title has no header of ours: it puts the number in r11. */
#define SC_PPU_THREAD_CREATE  41
#define SC_PPU_THREAD_EXIT    42
#define SC_PPU_THREAD_JOIN    44
#define SC_TIMER_USLEEP      141
#define SC_TTY_WRITE         403

/* Firmware import NIDs, computed from the export names by the algorithm in
 * include/ps3emu/nid.h -- the values a real title's import table carries, and
 * the ones tools/gen_hle_nids.py registers the handlers under. */
#define NID__cellGcmInitBody          0x15BAE46Bu
#define NID_cellGcmSetDisplayBuffer   0xA53D12AEu
#define NID_cellGcmGetControlRegister 0xA547ADDEu
#define NID_cellGcmSetFlipCommand     0x5C770579u
#define NID_cellGcmGetFlipStatus      0x72A577CEu
#define NID_cellGcmSetVBlankHandler   0xA91B0402u
#define NID_sys_process_exit          0xE6F2C1E7u

/* cellGcmSys.h's flip states. */
#define SMOKE_FLIP_DONE     0

/* Chosen so a run that reaches the exit call is distinguishable from every
 * accidental status the host could produce (0, 1, 2, a signal). */
#define SMOKE_EXIT_STATUS   0x27
/* What the guest thread exits with, and what the join must hand back. */
#define SMOKE_THREAD_STATUS 0x1234
#define SMOKE_THREAD_ARG    0x5A5A5A5Au

/* ---------------------------------------------------------------------------
 * Markers
 *
 * The ids are the order the run must happen in; the strings live in the image
 * at SMOKE_STRTAB + id * SMOKE_STR_STRIDE. An id with no string means this
 * enum and the list in tools/make_smoke_elf.py have drifted, and the guest
 * fails on that rather than printing nothing and passing.
 * -----------------------------------------------------------------------*/
enum {
    SMOKE_STEP_ENTRY = 0,
    SMOKE_STEP_IMAGE,
    SMOKE_STEP_HLE,
    SMOKE_STEP_THREAD_CREATE,
    SMOKE_STEP_THREAD_BODY,
    SMOKE_STEP_THREAD_JOIN,
    SMOKE_STEP_FRAME_1,
    SMOKE_STEP_FRAME_2,
    SMOKE_STEP_FRAME_3,
    SMOKE_STEP_VBLANK,
    SMOKE_STEP_EXIT,
    SMOKE_STEP_COUNT
};

#define SMOKE_MARK_CAP 32
static unsigned      g_marks[SMOKE_MARK_CAP];
static atomic_uint   g_mark_n;
/* Set by the creator once its thread-create marker is down. The new thread
 * can be scheduled before sys_ppu_thread_create even returns to its caller,
 * so without this handshake "thread-body" lands before "thread-create" on a
 * busy host and the strict marker order below is not the scaffold's to
 * guarantee. The body waits on it before it marks anything. */
static atomic_uint   g_create_marked;

/* Failures the guest found while running. Reported by smoke_verify, which is
 * the only thing that reads them. */
#define SMOKE_FAIL_CAP 16
static const char*   g_fail[SMOKE_FAIL_CAP];
static atomic_uint   g_fail_n;

static void smoke_fail(const char* what)
{
    unsigned i = atomic_fetch_add(&g_fail_n, 1u);
    if (i < SMOKE_FAIL_CAP) g_fail[i] = what;
}

static void smoke_check(int ok, const char* what)
{
    if (!ok) smoke_fail(what);
}

/* Host thread identities, so "the guest thread ran on a second host thread" is
 * a fact rather than a hope. */
static unsigned long g_main_host_tid;
static unsigned long g_body_host_tid;
static uint32_t      g_body_arg;

/* What the vblank handler saw: how many times it ran, on which host thread,
 * and the count the runtime handed it in r3. */
static atomic_uint   g_vblank_calls;
static unsigned long g_vblank_host_tid;
static uint32_t      g_vblank_last_count;

/* ---------------------------------------------------------------------------
 * Guest helpers
 * -----------------------------------------------------------------------*/

/* Record a step and print its line through sys_tty_write, in that order and
 * from one place: a marker cannot be recorded without the guest's output path
 * having run, and the log line cannot appear without the marker.
 *
 * Clobbers r3-r6 and r11 the way a lifted `sc` sequence does. r1, r2 and r13
 * are left alone -- the caller's stack, TOC and thread pointer. */
static void smoke_mark(ppu_context* ctx, unsigned step)
{
    unsigned i = atomic_fetch_add(&g_mark_n, 1u);
    if (i < SMOKE_MARK_CAP) g_marks[i] = step;

    uint32_t ea  = SMOKE_STRTAB + step * SMOKE_STR_STRIDE;
    uint32_t len = 0;
    while (len < SMOKE_STR_STRIDE - 1u && vm_read8(ea + len)) len++;
    if (!len) { smoke_fail("a marker step has no string in the image"); return; }

    ctx->gpr[11] = SC_TTY_WRITE;
    ctx->gpr[3]  = 0;                  /* channel: stdout */
    ctx->gpr[4]  = ea;
    ctx->gpr[5]  = len;
    ctx->gpr[6]  = SMOKE_SC_TTYLEN;    /* u32* bytes written */
    lv2_syscall(ctx);
    if ((int32_t)ctx->gpr[3] != 0) smoke_fail("sys_tty_write returned an error");
    if (vm_read32(SMOKE_SC_TTYLEN) != len)
        smoke_fail("sys_tty_write did not write back the byte count");
}

static void smoke_usleep(ppu_context* ctx, uint32_t us)
{
    ctx->gpr[11] = SC_TIMER_USLEEP;
    ctx->gpr[3]  = us;
    lv2_syscall(ctx);
}

/* One non-incrementing NV4097 method write, byte for byte what
 * runtime/host/host_posix.c's emit() produces -- except that the words are
 * stored with the lifted vm_write32, so they take the guest store path. */
static uint32_t g_fifo_len;

static void smoke_emit(uint32_t method, uint32_t data)
{
    vm_write32(SMOKE_IO_ADDR + g_fifo_len, (1u << 18) | (((method >> 2) & 0x7FFu) << 2));
    g_fifo_len += 4;
    vm_write32(SMOKE_IO_ADDR + g_fifo_len, data);
    g_fifo_len += 4;
}

/* ---------------------------------------------------------------------------
 * Firmware import stubs.
 *
 * These are what --hle-stubs turns a .lib.stub trampoline into: a function at
 * the stub's guest address whose entire body dispatches the NID. Arguments are
 * already in r3.. and the result comes back in r3, so the caller sets up the
 * registers and reads them back, exactly as lifted code does.
 * -----------------------------------------------------------------------*/
void func_00010200(ppu_context* ctx) { ps3_hle_call(NID__cellGcmInitBody, ctx); return; }
void func_00010300(ppu_context* ctx) { ps3_hle_call(NID_cellGcmSetDisplayBuffer, ctx); return; }
void func_00010400(ppu_context* ctx) { ps3_hle_call(NID_cellGcmGetControlRegister, ctx); return; }
void func_00010500(ppu_context* ctx) { ps3_hle_call(NID_cellGcmSetFlipCommand, ctx); return; }
void func_00010600(ppu_context* ctx) { ps3_hle_call(NID_cellGcmGetFlipStatus, ctx); return; }
void func_00010700(ppu_context* ctx) { ps3_hle_call(NID_sys_process_exit, ctx); return; }

/* The vblank handler the guest registers, which is what a title's frame
 * pacing hangs off. The harness's clock marks a beat pending from its own
 * thread; the runtime delivers it here, on the guest thread, at the next HLE
 * boundary, so this runs nested inside one of the status polls in the frame
 * loop, in a scratch context whose r3 is the vblank count. It records what it
 * saw and returns; a handler that called back into the runtime would be
 * testing something else. */
void func_00010800(ppu_context* ctx)
{
    g_vblank_last_count = (uint32_t)ctx->gpr[3];
    g_vblank_host_tid   = (unsigned long)GetCurrentThreadId();
    atomic_fetch_add(&g_vblank_calls, 1u);
}
void func_00010900(ppu_context* ctx) { ps3_hle_call(NID_cellGcmSetVBlankHandler, ctx); return; }

/* ---------------------------------------------------------------------------
 * The verdict.
 *
 * Runs from the host exit() inside sys_process_exit, installed immediately
 * before the guest makes that call so it is the FIRST atexit handler to run --
 * the frame clock and the RSX backend are still live at that moment, and
 * whatever they do while the process tears down is not this check's business.
 * -----------------------------------------------------------------------*/
static const char* const k_step_name[SMOKE_STEP_COUNT] = {
    "entry", "image", "hle", "thread-create", "thread-body",
    "thread-join", "frame-1", "frame-2", "frame-3", "vblank", "exit",
};

static void smoke_verify(void)
{
    unsigned n = atomic_load(&g_mark_n);
    unsigned frames = ppu_boot_frames_presented();
    unsigned flips  = cellGcm_flip_request_count();
    int ok = 1;

    printf("\n[smoke] --- verdict ---\n");

    /* Markers, in order and complete. */
    if (n != SMOKE_STEP_COUNT) {
        printf("[smoke] FAIL: %u marker(s), expected %u\n", n, SMOKE_STEP_COUNT);
        ok = 0;
    }
    for (unsigned i = 0; i < n && i < SMOKE_MARK_CAP; i++) {
        if (g_marks[i] != i) {
            printf("[smoke] FAIL: marker %u is \"%s\", expected \"%s\"\n", i,
                   g_marks[i] < SMOKE_STEP_COUNT ? k_step_name[g_marks[i]] : "?",
                   i < SMOKE_STEP_COUNT ? k_step_name[i] : "?");
            ok = 0;
        }
    }
    if (ok) printf("[smoke] OK: all %u markers, in order\n", n);

    /* The guest thread really was a second host thread. */
    printf("[smoke] host threads: main=%lu guest=%lu\n", g_main_host_tid, g_body_host_tid);
    if (!g_main_host_tid || !g_body_host_tid || g_main_host_tid == g_body_host_tid) {
        printf("[smoke] FAIL: the guest thread did not run on its own host thread\n");
        ok = 0;
    }
    if (g_body_arg != SMOKE_THREAD_ARG) {
        printf("[smoke] FAIL: the guest thread saw arg 0x%08X, expected 0x%08X\n",
               g_body_arg, SMOKE_THREAD_ARG);
        ok = 0;
    }

    /* Frames, both requested by the guest and presented by the backend. */
    printf("[smoke] flips requested by the guest: %u; frames presented: %u\n",
           flips, frames);
    if (flips != SMOKE_FRAMES) {
        printf("[smoke] FAIL: %u flip request(s), expected %d\n", flips, SMOKE_FRAMES);
        ok = 0;
    }
    if (frames < SMOKE_FRAMES) {
        printf("[smoke] FAIL: the backend presented %u frame(s), expected %d\n",
               frames, SMOKE_FRAMES);
        ok = 0;
    }

    /* The vblank handler: delivered on the guest thread, with the count. */
    {
        unsigned calls = atomic_load(&g_vblank_calls);
        printf("[smoke] vblank handler: %u call(s), last count %u, host thread %lu\n",
               calls, g_vblank_last_count, g_vblank_host_tid);
        if (!calls) {
            printf("[smoke] FAIL: the guest's vblank handler never ran\n");
            ok = 0;
        } else if (g_vblank_host_tid != g_main_host_tid) {
            printf("[smoke] FAIL: the vblank handler ran on host thread %lu, not the guest's %lu\n",
                   g_vblank_host_tid, g_main_host_tid);
            ok = 0;
        } else if (!g_vblank_last_count) {
            printf("[smoke] FAIL: the vblank handler was handed count 0\n");
            ok = 0;
        }
    }

    /* The chosen exit status made it through sys_process_exit. */
    if (!g_sys_process_exit_called) {
        printf("[smoke] FAIL: sys_process_exit never ran\n");
        ok = 0;
    } else if (g_sys_process_exit_code != SMOKE_EXIT_STATUS) {
        printf("[smoke] FAIL: sys_process_exit got status %d, expected %d\n",
               g_sys_process_exit_code, SMOKE_EXIT_STATUS);
        ok = 0;
    } else {
        printf("[smoke] OK: sys_process_exit received status %d\n",
               g_sys_process_exit_code);
    }

    /* Anything the guest itself found wrong on the way. */
    {
        unsigned f = atomic_load(&g_fail_n);
        for (unsigned i = 0; i < f && i < SMOKE_FAIL_CAP; i++)
            printf("[smoke] FAIL: %s\n", g_fail[i]);
        if (f) ok = 0;
    }

    printf("[smoke] %s\n", ok ? "PASS" : "FAILED");
    fflush(stdout);
    fflush(stderr);
    /* Override the guest's status with this harness's verdict: the status
     * itself is one of the things being checked, so it must not also be the
     * channel the result comes back on. */
    _exit(ok ? 0 : 1);
}

/* ---------------------------------------------------------------------------
 * The guest thread body. sys_ppu_thread_create was given the OPD at
 * SMOKE_OPD_THREAD; the runtime resolved it and dispatched here on a fresh
 * host thread, with the creator's argument in r3.
 * -----------------------------------------------------------------------*/
void func_00010100(ppu_context* ctx)
{
    g_body_host_tid = (unsigned long)GetCurrentThreadId();
    g_body_arg      = (uint32_t)ctx->gpr[3];
    /* Let the creator mark first (see g_create_marked); a sleep through the
     * timer syscall rather than a spin, so the wait is itself guest-shaped. */
    while (!atomic_load(&g_create_marked)) smoke_usleep(ctx, 100);
    smoke_mark(ctx, SMOKE_STEP_THREAD_BODY);

    smoke_check(ctx->gpr[13] != 0, "the guest thread has no thread pointer in r13");

    ctx->gpr[11] = SC_PPU_THREAD_EXIT;
    ctx->gpr[3]  = SMOKE_THREAD_STATUS;
    lv2_syscall(ctx);   /* does not return: the runtime unwinds the thread */
}

/* ---------------------------------------------------------------------------
 * _start. ppu_run resolved the entry OPD out of the loaded image and called
 * this with the PS3 process-entry register state: r1 stack, r2 TOC, r3/r4
 * argc/argv, r13 the thread pointer.
 * -----------------------------------------------------------------------*/
void func_00010000(ppu_context* ctx)
{
    uint32_t argc = (uint32_t)ctx->gpr[3];
    uint32_t argv = (uint32_t)ctx->gpr[4];
    uint32_t toc  = (uint32_t)ctx->gpr[2];
    uint32_t tp   = (uint32_t)ctx->gpr[13];
    uint32_t gcm_ctx, ctrl;
    uint64_t tid;

    g_main_host_tid = (unsigned long)GetCurrentThreadId();
    smoke_mark(ctx, SMOKE_STEP_ENTRY);

    /* The entry state ppu_run built. argv[0] is a big-endian 8-byte slot whose
     * pointer sits in the low word, which is where the PS3 CRT reads it. */
    smoke_check(toc == SMOKE_DATA_BASE + 0x0D00u, "r2 is not this image's TOC");
    smoke_check(argc == 1, "argc is not 1");
    smoke_check(argv != 0 && vm_read8(vm_read32(argv + 4)) != 0,
                "argv[0] does not point at a string");

    /* The image itself: initialised data, the BSS tail past filesz, and the
     * PT_TLS template ppu_run copied into the main thread's TLS block. */
    smoke_check(vm_read32(SMOKE_CANARY_EA) == SMOKE_CANARY,
                "the image canary is not in guest RAM (PT_LOAD did not land)");
    vm_write32(SMOKE_SC_BSSPROBE, SMOKE_CANARY);
    smoke_check(vm_read32(SMOKE_SC_BSSPROBE) == SMOKE_CANARY,
                "the BSS tail past filesz is not backed guest RAM");
    smoke_check(tp != 0 && vm_read32(tp - SMOKE_TLS_TP_BIAS) == SMOKE_TLS_CANARY,
                "r13 does not point at the loaded PT_TLS template");
    smoke_mark(ctx, SMOKE_STEP_IMAGE);

    /* A firmware import, through the NID bridge. _cellGcmInitBody is the call
     * the SDK's cellGcmInit() macro makes, and it is what brings up the GCM
     * context the frame loop below writes into.
     *   r3 = &context (out), r4 = cmdSize, r5 = ioSize, r6 = ioAddress */
    ctx->gpr[3] = SMOKE_SC_GCMCTX;
    ctx->gpr[4] = SMOKE_CMD_SIZE;
    ctx->gpr[5] = SMOKE_IO_SIZE;
    ctx->gpr[6] = SMOKE_IO_ADDR;
    func_00010200(ctx); DRAIN_TRAMPOLINE(ctx);
    smoke_check((int32_t)ctx->gpr[3] == 0, "_cellGcmInitBody returned an error");
    gcm_ctx = vm_read32(SMOKE_SC_GCMCTX);
    smoke_check(gcm_ctx != 0, "_cellGcmInitBody left the context pointer null");
    smoke_check(vm_read32(gcm_ctx + 0) == SMOKE_IO_ADDR,
                "the GCM context does not begin at the IO address");
    smoke_mark(ctx, SMOKE_STEP_HLE);

    /* A guest thread. The entry is an OPD address, which the runtime's thread
     * trampoline resolves the same way ppu_run resolved ours. */
    ctx->gpr[11] = SC_PPU_THREAD_CREATE;
    ctx->gpr[3]  = SMOKE_SC_TID;          /* u64* thread id (out) */
    ctx->gpr[4]  = SMOKE_OPD_THREAD;      /* entry OPD           */
    ctx->gpr[5]  = SMOKE_THREAD_ARG;      /* the new thread's r3 */
    ctx->gpr[6]  = 1000;                  /* priority            */
    ctx->gpr[7]  = 0x10000;               /* stack size          */
    ctx->gpr[8]  = 0;                     /* flags               */
    ctx->gpr[9]  = SMOKE_THREAD_NAME;
    lv2_syscall(ctx);
    smoke_check((int32_t)ctx->gpr[3] == 0, "sys_ppu_thread_create failed");
    tid = vm_read64(SMOKE_SC_TID);
    smoke_check(tid != 0, "sys_ppu_thread_create returned thread id 0");
    smoke_mark(ctx, SMOKE_STEP_THREAD_CREATE);
    atomic_store(&g_create_marked, 1u);

    ctx->gpr[11] = SC_PPU_THREAD_JOIN;
    ctx->gpr[3]  = tid;
    ctx->gpr[4]  = SMOKE_SC_EXITSTAT;     /* s64* exit status (out) */
    lv2_syscall(ctx);
    smoke_check((int32_t)ctx->gpr[3] == 0, "sys_ppu_thread_join failed");
    smoke_check((int64_t)vm_read64(SMOKE_SC_EXITSTAT) == SMOKE_THREAD_STATUS,
                "sys_ppu_thread_join handed back the wrong exit status");
    smoke_mark(ctx, SMOKE_STEP_THREAD_JOIN);

    /* Give the flip a display buffer to land on, then take the control
     * register's guest address: put and get live there, and writing put is how
     * a title tells the RSX there is work.
     *   cellGcmSetDisplayBuffer(id, offset, pitch, width, height) */
    ctx->gpr[3] = 0;
    ctx->gpr[4] = 0;
    ctx->gpr[5] = 1280 * 4;
    ctx->gpr[6] = 1280;
    ctx->gpr[7] = 720;
    func_00010300(ctx); DRAIN_TRAMPOLINE(ctx);
    smoke_check((int32_t)ctx->gpr[3] == 0, "cellGcmSetDisplayBuffer returned an error");

    func_00010400(ctx); DRAIN_TRAMPOLINE(ctx);
    ctrl = (uint32_t)ctx->gpr[3];
    smoke_check(ctrl != 0, "cellGcmGetControlRegister returned null");

    /* Register the vblank handler before the first frame: the frame loop's
     * status polls are the HLE boundaries the runtime delivers it at. The
     * argument is the handler's OPD, as a title passes it. */
    ctx->gpr[3] = SMOKE_OPD_VBLANK;
    func_00010900(ctx); DRAIN_TRAMPOLINE(ctx);

    /* Three frames. Each one appends its commands to the ring, publishes put,
     * asks for a flip and waits for the flip to complete -- the shape of a
     * title's frame loop, and the reason the harness's frame clock has to be
     * running for any of it to finish. */
    for (int frame = 0; frame < SMOKE_FRAMES; frame++) {
        int waited;

        smoke_emit(NV4097_SET_COLOR_CLEAR_VALUE, SMOKE_CLEAR_ARGB);
        smoke_emit(NV4097_CLEAR_SURFACE,         0xF0u);
        vm_write32(ctrl + 0, g_fifo_len);        /* put: the RSX chases this */

        ctx->gpr[3] = 0;                         /* buffer id */
        func_00010500(ctx); DRAIN_TRAMPOLINE(ctx);
        smoke_check((int32_t)ctx->gpr[3] == 0, "cellGcmSetFlipCommand returned an error");

        /* The flip completes on the harness's vblank beat, delivered on this
         * thread at the next HLE boundary -- which is the status poll itself.
         * Bounded so a stalled clock fails the run instead of hanging it. */
        for (waited = 0; waited < 2000; waited++) {
            func_00010600(ctx); DRAIN_TRAMPOLINE(ctx);
            if ((uint32_t)ctx->gpr[3] == SMOKE_FLIP_DONE) break;
            smoke_usleep(ctx, 1000);
        }
        if (waited >= 2000) smoke_fail("a flip never completed");
        smoke_mark(ctx, SMOKE_STEP_FRAME_1 + frame);
    }

    /* The RSX walker consumed everything the guest wrote: get caught up with
     * put. Without this the frames above prove only that the flips were
     * counted, not that the command stream was read. */
    {
        int waited;
        for (waited = 0; waited < 2000; waited++) {
            if (vm_read32(ctrl + 4) == g_fifo_len) break;
            smoke_usleep(ctx, 1000);
        }
        smoke_check(vm_read32(ctrl + 4) == g_fifo_len,
                    "the RSX never drained the guest's command stream (get != put)");
    }

    /* The vblank handler ran. Three frames of polling is normally beats
     * enough, and the poll is itself the boundary that delivers a pending
     * one, so keep polling, bounded, until it has. The verdict checks what
     * the handler recorded: that it ran on this thread with a count in r3. */
    {
        int waited;
        for (waited = 0; waited < 2000; waited++) {
            if (atomic_load(&g_vblank_calls) != 0) break;
            func_00010600(ctx); DRAIN_TRAMPOLINE(ctx);
            smoke_usleep(ctx, 1000);
        }
        if (waited >= 2000) smoke_fail("the guest's vblank handler never ran");
    }
    smoke_mark(ctx, SMOKE_STEP_VBLANK);

    /* A flip reads as done the moment the clock ticks it; the backend presents
     * it a step later on the clock's own thread. A guest that exits in that
     * gap takes the process down with the last frame still unpresented, and
     * the verdict below then counts one present too few -- which is what a
     * loaded runner showed. Give the clock the beat it needs, bounded like the
     * waits above, so the verdict measures the backend and not the race. */
    {
        int waited;
        for (waited = 0; waited < 2000; waited++) {
            if (ppu_boot_frames_presented() >= (unsigned)SMOKE_FRAMES) break;
            smoke_usleep(ctx, 1000);
        }
    }

    smoke_mark(ctx, SMOKE_STEP_EXIT);

    /* Installed last so it runs first: exit() calls handlers in reverse order
     * of registration, and this one must get its verdict out while the rest of
     * the process is still standing. */
    atexit(smoke_verify);

    ctx->gpr[3] = SMOKE_EXIT_STATUS;
    func_00010700(ctx); DRAIN_TRAMPOLINE(ctx);

    /* sys_process_exit does not return. Reaching here means it did. */
    smoke_fail("sys_process_exit returned");
    smoke_verify();
}

/* ---------------------------------------------------------------------------
 * The lifted function table. ppu_recomp_register() (runtime/ppu/ppu_loader.cpp)
 * walks this into the address -> function map that ps3_indirect_call, the
 * thread trampoline and ppu_run all resolve through.
 * -----------------------------------------------------------------------*/
const func_entry function_table[] = {
    { 0x00010000ULL, func_00010000, "func_00010000" },
    { 0x00010100ULL, func_00010100, "func_00010100" },
    { 0x00010200ULL, func_00010200, "func_00010200" },
    { 0x00010300ULL, func_00010300, "func_00010300" },
    { 0x00010400ULL, func_00010400, "func_00010400" },
    { 0x00010500ULL, func_00010500, "func_00010500" },
    { 0x00010600ULL, func_00010600, "func_00010600" },
    { 0x00010700ULL, func_00010700, "func_00010700" },
    { 0x00010800ULL, func_00010800, "func_00010800" },
    { 0x00010900ULL, func_00010900, "func_00010900" },
    { 0, NULL, NULL }
};
const uint64_t function_table_count = 10;

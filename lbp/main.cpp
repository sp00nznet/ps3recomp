/*
 * ps3recomp - integrated PPU boot harness (first-boot attempt).
 *
 * Links the whole PPU runtime half into one executable and starts executing
 * the recompiled game's entry point:
 *
 *   lifted code (ppu_recomp.c) + loader (ppu_loader.cpp) + HLE bridge
 *   (ppu_hle.cpp + generated NID table) + HLE libs (cellGcmSys, rsx_commands)
 *
 * It loads the real EBOOT image, registers the lifted functions and the HLE
 * NID handlers, then dispatches the entry. Execution runs real Uncharted boot
 * code until it reaches a function outside the lifted subset (logged by the
 * unlifted stub), an unimplemented firmware import (logged by ps3_hle_call),
 * or an lv2 syscall (logged by lv2_syscall) -- telling us exactly what to
 * implement next.
 *
 * This proves the integration builds + runs; a full-image build additionally
 * needs the lifter to split output into multiple TUs (88 MB single-file
 * otherwise).
 */
#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
uint32_t ppu_load_elf(const char* path);
void     ppu_recomp_register(void);
void     ppu_hle_init(void);
void     ppu_sysprx_register(void);
void     ppu_fs_register(void);
extern "C" void cellSaveData_register_ctx_handlers(void);  /* libs/system/cellSaveData.c */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
void     lbp_hang_census(void);    /* diagnostic: LBP resource-pump queue census */
void     lbp_unstick_once(void);   /* diagnostic: drain stuck loader queues + nudge */
extern const char* ppu_vfs_root;   /* host dir that PS3 mount points map into */
extern "C" void cellGame_init_from_paramsfo(const char* sfo_path);  /* real title id (BCUS98148) */
/* Optional hook: load real system PRX modules (libsre = cellSpurs/cellSync) into
 * guest RAM and register their exports. Weak default is a no-op; a title that
 * links a lifted PRX defines a strong version. Called after the lifted function
 * table is registered and vm_base is live, before the game runs. */
void     ps3_load_prx_modules(void) __attribute__((weak));
void     ps3_load_prx_modules(void) {}
}

#include <string.h>
#include <stdlib.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
/* Last-chance crash reporter: vm_base accesses are bounds-guarded, so a real
 * access violation means a HOST pointer deref (e.g. a bad function pointer or a
 * runtime-struct walk). Print the faulting address and the RIP as a module
 * offset (RVA) so it can be symbolized with llvm-symbolizer against the PDB. */
extern "C" uint32_t    g_last_hle_nid;    /* ppu_hle.cpp breadcrumb */
extern "C" const char* g_last_hle_name;

extern "C" __declspec(thread) ppu_context* g_active_ctx;
static LONG WINAPI ydkj_crash_filter(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    fprintf(stderr, "\n[CRASH] code=0x%08lX rip=%p\n",
            (unsigned long)er->ExceptionCode, er->ExceptionAddress);
    fprintf(stderr, "[CRASH] last HLE NID 0x%08X (%s)\n",
            g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, "[CRASH] %s fault address 0x%llX\n",
                er->ExceptionInformation[0] ? "write" : "read",
                (unsigned long long)er->ExceptionInformation[1]);
    if (g_active_ctx) fprintf(stderr, "[CRASH] guest ctr=0x%08X lr=0x%08X r3=0x%08X\n",
          (uint32_t)g_active_ctx->ctr, (uint32_t)g_active_ctx->lr, (uint32_t)g_active_ctx->gpr[3]);
    HMODULE mod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)er->ExceptionAddress, &mod);
    fprintf(stderr, "[CRASH] module=%p rva=0x%llX  (llvm-symbolizer --obj=minecraft.exe 0x%llX)\n",
            (void*)mod, (unsigned long long)((char*)er->ExceptionAddress - (char*)mod),
            (unsigned long long)((char*)er->ExceptionAddress - (char*)mod));
    /* Host call stack (RVAs) so the lifted caller can be symbolized. */
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, frames, NULL);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        if (m == mod)
            fprintf(stderr, "[CRASH]   #%-2u rva=0x%llX\n", i,
                    (unsigned long long)((char*)frames[i] - (char*)m));
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#ifdef _WIN32
/* abort()/exit(3) reporter: the recompiled CRT (or a failed invariant) can call
 * abort() — Windows turns that into exit code 3 with no message. Capture a host
 * backtrace (RVAs) + the last HLE NID so the aborting caller can be symbolized. */
static void ydkj_abort_handler(int)
{
    fprintf(stderr, "\n[ABORT] SIGABRT raised; last HLE NID 0x%08X (%s)\n",
            g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    void* frames[32];
    USHORT n = RtlCaptureStackBackTrace(0, 32, frames, NULL);
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ydkj_abort_handler, &self);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        if (m == self)
            fprintf(stderr, "[ABORT]   #%-2u rva=0x%llX\n", i,
                    (unsigned long long)((char*)frames[i] - (char*)m));
    }
    fflush(stderr);
    _exit(3);
}
#endif

/* Derive the VFS root (the dir containing PS3_GAME) from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf  -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];
static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }
    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip three trailing components: EBOOT.elf / USRDIR / PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* Host-provided symbols the runtime + HLE libs need. */
extern "C" uint8_t* vm_base = nullptr;

/* Lifted wwsjob policy-module entry (lbp_spu/lifted/pm_wwsjob) + the workload
 * registry hook, for the raw-fingerprint alias registered in main(). */
struct spu_context;
extern "C" void pm_wwsjob_spu_func_00000A00(struct spu_context*);
/* Lifted WWS "JOBCRT Ver13" job binary (lbp_spu/lifted/job_wws_7BD900). Raw
 * blob at EA 0x7BD900, NOT an ELF -- extract_spu_images.py only finds ELF-magic
 * images, so it was wrapped into an ELF by hand. It is now picked up and lifted
 * by build_spu_workloads.py like any other image (it used to be lifted by hand
 * under a "jobwws_" prefix and registered here as image 90; that duplicate is
 * gone). Only the RAW-blob fingerprint alias below is still needed. */
extern "C" void job_wws_7BD900_spu_func_00000000(struct spu_context*);
extern "C" void spu_begin_image(int image_id);
extern "C" void spu_workload_register_img(unsigned long long,
        void (*)(struct spu_context*), int, const char*);
extern "C" uint32_t ppu_vm_size;   /* defined in ppu_loader.cpp (OOB guard) */
extern "C" void lv2_init_syscalls(void);   /* runtime/syscalls/lv2_register.c */

/* Guest-callback dispatch + RSX vblank/flip driver.
 *
 * g_ps3_guest_caller (defined NULL by libs/system/cellSysutil.c) is the hook the
 * HLE runtime uses to call back into recompiled code -- cellSysutil events and
 * the GCM vblank/flip handlers. ppu_guest_call (ppu_loader.cpp) does the OPD ->
 * dispatch. On real hardware the RSX fires a vblank interrupt ~60x/s that drives
 * the game's frame loop; with no RSX we synthesize it from a host timer thread
 * calling cellGcmTickVBlank()/TickFlip(), which invoke the registered handlers.
 * Without this the game inits, registers its handlers, and then waits forever
 * for a vblank that never comes. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" ps3_guest_caller_fn g_ps3_guest_caller;        /* libs/system/cellSysutil.c */
extern "C" uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" void cellGcmTickVBlank(void);
extern "C" void cellGcmTickFlip(void);

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{ ppu_guest_call(opd, a0, a1, a2, a3); }

#ifdef _WIN32
/* RSX present backend (libs/video/rsx_d3d12_backend.c). Driven on the vblank
 * thread so the D3D12 device + window message pump live on one thread. */
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" void cellGcm_rsx_process_fifo(void);   /* cellGcmSys.c: drain get->put */
extern "C" void* cellGcm_fifo_kick_event(void);   /* cellGcmSys.c: dry-queue kick */
extern "C" unsigned long long ps3_ms_now(void);   /* runtime: monotonic ms clock  */

#pragma comment(lib, "winmm")
/* ---- Guest-PC sampling profiler (PS3_GUEST_PROF=1) -----------------------
 * Samples every guest thread's ctx.cia every ~5ms and dumps the top sites
 * every 5s. cia is refreshed at every syscall, and guest spin loops issue a
 * syscall per iteration (yield/usleep), so a thread stuck in a wait loop
 * shows its loop's PC dominating the histogram. This is the tool that
 * answers "where does thread X spend its wall time" without a debugger. */
extern "C" int ppu_prof_snapshot(int idx, unsigned* tid, unsigned* cia, const char** name);
#ifndef PPU_THREAD_MAX
#define PPU_THREAD_MAX 64
#endif
static const char* prof_name(unsigned tid) {
    static const char* nm; unsigned t, c;
    nm = "";
    ppu_prof_snapshot((int)tid - 1, &t, &c, &nm);
    return nm;
}
static DWORD WINAPI guest_prof_thread(LPVOID)
{
    struct Site { uint32_t tid, cia; uint32_t hits; };
    static Site sites[256];
    int nsites = 0;
    ULONGLONG t0 = GetTickCount64();
    unsigned total = 0;
    for (;;) {
        Sleep(5);
        static int s_focus = -2;
        if (s_focus == -2) { const char* e = getenv("PS3_GUEST_PROF_TID"); s_focus = e ? atoi(e) : -1; }
        for (int i = 0; i < PPU_THREAD_MAX; i++) {
            unsigned tid, cia; const char* nm;
            if (!ppu_prof_snapshot(i, &tid, &cia, &nm)) continue;
            if (!cia) continue;
            if (s_focus >= 0 && (int)tid != s_focus) continue;
            total++;
            int f = -1;
            for (int k = 0; k < nsites; k++)
                if (sites[k].tid == tid && sites[k].cia == cia) { f = k; break; }
            if (f < 0 && nsites < 256) { f = nsites++; sites[f].tid = tid; sites[f].cia = cia; sites[f].hits = 0; }
            if (f >= 0) sites[f].hits++;
        }
        ULONGLONG now = GetTickCount64();
        if (now - t0 >= 5000) {
            /* top 10 by hits */
            fprintf(stderr, "[gprof] %u samples/5s; top sites:\n", total);
            for (int rank = 0; rank < 10; rank++) {
                int best = -1; uint32_t bh = 0;
                for (int k = 0; k < nsites; k++)
                    if (sites[k].hits > bh) { bh = sites[k].hits; best = k; }
                if (best < 0 || bh == 0) break;
                fprintf(stderr, "[gprof]   tid=%-3u cia=0x%08X  %5.1f%%  (%s)\n",
                        sites[best].tid, sites[best].cia,
                        100.0 * sites[best].hits / (total ? total : 1),
                        prof_name(sites[best].tid));
                sites[best].hits = 0;   /* consume */
            }
            fflush(stderr);
            nsites = 0; total = 0; t0 = now;
        }
    }
}

static DWORD WINAPI vblank_ticker(LPVOID)
{
    int rsx_ok = (rsx_d3d12_backend_init(1280, 720, "LittleBigPlanet (ps3recomp)") == 0);
    fprintf(stderr, "[rsx] backend init %s\n", rsx_ok ? "OK -- window open" : "FAILED");
    /* Sleep(16) with the default 15.6 ms timer granularity actually sleeps
     * ~31 ms -> the "60 Hz" vblank ran at 32.5 Hz (measured, LBP_PACE) and the
     * game's whole boot marched at half speed: LBP steps its loading pipeline
     * once per vblank-paced frame, so slow ticks LOOK like a boot stall.
     * timeBeginPeriod(1) makes Sleep millisecond-accurate; the absolute-
     * deadline loop then holds a true 60 Hz without drift. */
    timeBeginPeriod(1);
    ULONGLONG next_ms = GetTickCount64();
    /* Fence-kick event: a game thread whose cellGcmFinish wait finds the
     * fence queue dry signals this thread to drain the FIFO immediately
     * instead of on the next 16 ms tick. Presents stay on the 60 Hz cadence;
     * only the drain runs early. Without this a movie frame's ~46 one-ahead
     * finish waits paid a full tick each (~0.75 s per movie frame). */
    HANDLE kick_ev = (HANDLE)cellGcm_fifo_kick_event();
    for (;;) {
        next_ms += 16;        /* 60 Hz cadence against an absolute deadline */
        for (;;) {
            ULONGLONG now = GetTickCount64();
            if (now >= next_ms) { next_ms = now > next_ms + 16 ? now : next_ms; break; }
            DWORD w = WaitForSingleObject(kick_ev, (DWORD)(next_ms - now));
            if (w == WAIT_OBJECT_0 && rsx_ok)
                cellGcm_rsx_process_fifo();   /* early drain: retire fences now */
            else if (w != WAIT_OBJECT_0)
                break;                        /* deadline reached */
        }
        cellGcmTickVBlank();
        cellGcmTickFlip();
        if (rsx_ok) {
            if (rsx_d3d12_backend_pump_messages() != 0) { rsx_ok = 0; }
            /* LBP_PACE: the whole frame pipeline (vblank handlers, FIFO drain,
             * present) is serialized on this thread at a 16ms base period. If
             * drain or present overruns, the effective vblank rate drops and
             * the game's ENTIRE boot pace drops with it (the loading pipeline
             * advances a step per frame; at 10 Hz a 30 s boot takes minutes and
             * looks like a nondeterministic stall). Measure where the time goes. */
            static int s_pace = -1;
            if (s_pace < 0) s_pace = getenv("LBP_PACE") ? 1 : 0;
            if (s_pace) {
                static unsigned long long t0 = 0, drain_ms = 0, present_ms = 0;
                static unsigned frames = 0;
                unsigned long long a = ps3_ms_now();
                cellGcm_rsx_process_fifo();
                unsigned long long b = ps3_ms_now();
                rsx_d3d12_backend_present();
                unsigned long long c = ps3_ms_now();
                drain_ms += b - a; present_ms += c - b; frames++;
                if (t0 == 0) t0 = a;
                if (c - t0 >= 2000) {
                    fprintf(stderr, "[PACE] frames/s=%.1f drain=%llums/s present=%llums/s\n",
                            frames * 1000.0 / (c - t0),
                            (unsigned long long)(drain_ms * 1000 / (c - t0)),
                            (unsigned long long)(present_ms * 1000 / (c - t0)));
                    t0 = c; drain_ms = present_ms = 0; frames = 0;
                }
            } else {
                cellGcm_rsx_process_fifo();  /* execute the game's GCM commands */
                rsx_d3d12_backend_present(); /* present the frame */
            }
        }
    }
    return 0;
}

extern "C" uint32_t    g_last_hle_nid;
extern "C" const char* g_last_hle_name;
#include <tlhelp32.h>
/* When the boot wedges, snapshot every other thread's instruction pointer as a
 * module RVA (symbolize with llvm-symbolizer) so a guest spin/wait is pinned to
 * an exact lifted function -- the HLE breadcrumb only covers HLE calls. */
/* Snapshot every other thread's RIP. For threads in the boot module (lifted
 * guest code) print the RVA (symbolizable) + a couple of stack-return RVAs;
 * for threads parked in a DLL (OS waits / FMOD) print the module name so they
 * are not mistaken for guest spins. Called twice so the caller can diff which
 * guest thread is genuinely parked (same RIP) vs. still progressing. */
/* Scan a suspended thread's stack for boot-module return addresses to
 * reconstruct the call chain (some false positives expected). Bound to the
 * committed readable region: an unchecked scan AVs on guard pages / garbage
 * Rsp (thread mid-create) and the crash filter then kills the whole run. */
static void scan_ret_rvas(HMODULE self, const CONTEXT* tc, int cap)
{
    uint64_t* sp = (uint64_t*)tc->Rsp;
    uint64_t region_end = (uint64_t)sp;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)sp, &mbi, sizeof mbi) &&
        mbi.State == MEM_COMMIT &&
        !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        region_end = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
    int maxk = (int)((region_end - (uint64_t)sp) / 8);
    if (maxk > 0x8000 / 8) maxk = 0x8000 / 8;
    int found = 0;
    for (int k = 0; k < maxk && found < cap; k++) {
        uint64_t v = sp[k];
        if (v < (uint64_t)self) continue;
        HMODULE mm = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)v, &mm);
        if (mm == self) {
            fprintf(stderr, "[WATCHDOG]       ret rva=0x%llX\n",
                    (unsigned long long)(v - (uint64_t)self));
            found++;
        }
    }
}

static void dump_threads(const char* label, HMODULE self)
{
    fprintf(stderr, "[WATCHDOG] %s; last HLE call = 0x%08X (%s)\n",
            label, g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    /* LBP resource-pump census: dump the 3 work-queue depths + their semaphore
     * runtime values, to tell "work stranded" from "queues drained / completion
     * never signalled". Env-gated so it only runs under a debugging session. */
    if (getenv("HANGCENSUS")) lbp_hang_census();
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;
            SuspendThread(th);
            CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &ctx)) {
                HMODULE m = NULL;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)ctx.Rip, &m);
                if (m == self) {
                    fprintf(stderr, "[WATCHDOG]   tid %5lu BOOT rip rva=0x%llX\n",
                            (unsigned long)te.th32ThreadID,
                            (unsigned long long)((char*)ctx.Rip - (char*)self));
                    scan_ret_rvas(self, &ctx, 16);
                } else {
                    char path[MAX_PATH] = "?";
                    if (m) GetModuleFileNameA(m, path, sizeof path);
                    const char* base = strrchr(path, '\\');
                    fprintf(stderr, "[WATCHDOG]   tid %5lu in %s\n",
                            (unsigned long)te.th32ThreadID, base ? base + 1 : path);
                    /* LBP_WD_DLLSTACKS: a thread parked in an OS wait is only
                     * identifiable by WHO CALLED the wait -- print a few boot-
                     * module return RVAs from its stack (silent SPU wedges). */
                    if (getenv("LBP_WD_DLLSTACKS")) scan_ret_rvas(self, &ctx, 4);
                }
            }
            ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    fflush(stderr);
}

static DWORD WINAPI hang_watchdog(LPVOID)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&hang_watchdog, &self);
    Sleep(8000);
    dump_threads("8s sample", self);
    Sleep(7000);
    dump_threads("15s sample", self);
    /* LBP_WATCHDOG_PERIOD=<sec>: keep sampling -- late wedges (the FMOD
     * mixer's silent post-DSP loop at ~20s+) happen after both boot samples. */
    { const char* p = getenv("LBP_WATCHDOG_PERIOD");
      if (p) { int s = atoi(p); if (s < 1) s = 5;
        for (;;) { Sleep((DWORD)s * 1000); dump_threads("periodic", self); } } }
    return 0;
}

/* UNSTICK: continuously drain stuck loader queues + nudge the completion sem, to
 * test whether the nondeterministic hang is purely lost wakeups (see
 * lbp_unstick_once). If the title reaches the loading screen with this on, the
 * root cause is a racy producer/consumer handshake, not missing work. */
static DWORD WINAPI unstick_thread(LPVOID)
{
    for (;;) { Sleep(150); lbp_unstick_once(); }
}
#endif

/* The flat VM treats every address as valid RAM, so it must span every region
 * the PS3 memory map uses. The game's heap maps at 0x20000000+ and reaches
 * ~0x50000000, but sys_ppu_thread_create allocates thread stacks in the PS3
 * stack region at 0xD0000000-0xDFFFFFFF (vm.h: VM_STACK_BASE). Without covering
 * that, every spawned thread's stack access is OOB (reads 0 / writes dropped)
 * and the thread crashes. Size to include the stack region: ~3.75 GB, lazily
 * committed by the OS (only touched pages are backed). */
#define VM_SIZE    0x100010000ull /* full 32-bit guest space + 64K guard (top-edge reads), demand-committed */
#define STACK_TOP  0x0FF00000u   /* main-thread stack, below the 0x10000000 segment */

#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the full 4 GB guest space up front (no
 * commit cost) and commit each 64 KB page on first access. This makes EVERY
 * 32-bit guest offset valid -- a garbage guest pointer reads as zero instead of
 * crashing the process (essential now that the recompiled engine runs deep and
 * worker threads touch incomplete state). Out-of-arena faults fall through to
 * the crash reporter. */
/* Page-commit census (VM_TRACE=1). Committed pages are never released, so a
 * runaway toucher shows up as unbounded process memory with no host leak. Logs
 * the commit rate + the faulting GUEST address so the sweeper can be named:
 * a real working set is a few hundred MB (PS3 has 256 MB of RAM), so anything
 * approaching the 4 GB reservation means something is walking the address
 * space rather than using it. */
static volatile long g_vm_commits = 0;

/* Committed-page bitmap: one bit per 64 KB page across the 4 GB guest space
 * (8 KB total), set on every demand-commit below. The MFC DMA guard
 * (mfc_ea_range_committed) tests these bits instead of calling VirtualQuery
 * per transfer -- the syscall version measured 94 CPU-s in a 50 s run. */
extern "C" uint8_t g_vm_page_bitmap[65536 / 8];
uint8_t g_vm_page_bitmap[65536 / 8];

static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE)) {
                { uint32_t g = (uint32_t)(fault - base);
                  g_vm_page_bitmap[g >> 19] |= (uint8_t)(1u << ((g >> 16) & 7)); }
                static int s_tr = -1;
                if (s_tr < 0) s_tr = getenv("VM_TRACE") ? 1 : 0;
                if (s_tr) {
                    long n = _InterlockedIncrement(&g_vm_commits);
                    if (n <= 32 || (n & 0x3FF) == 0)
                        fprintf(stderr, "[VM] commit #%ld (%ld MB) guest=0x%08X %s\n",
                                n, n / 16, (uint32_t)(fault - base),
                                ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
                    /* Name the sweeper. A sane working set is a few hundred MB, so
                     * once we are thousands of pages in, whoever is still faulting
                     * IS the runaway. Dump the faulting RIP + host stack (RVAs, so
                     * llvm-symbolizer / the .map names the lifted function) and the
                     * live guest context. Once only -- this runs inside the VEH. */
                    if (n == 3000) {
                        char* mb = (char*)GetModuleHandleA(NULL);
                        fprintf(stderr, "\n[VM-SWEEP] runaway toucher at commit #%ld guest=0x%08X\n"
                                        "[VM-SWEEP]   faulting rip rva=0x%llX\n",
                                n, (uint32_t)(fault - base),
                                (unsigned long long)((char*)ep->ContextRecord->Rip - mb));
                        void* fr[32];
                        USHORT got = RtlCaptureStackBackTrace(0, 32, fr, NULL);
                        fprintf(stderr, "[VM-SWEEP]   host-bt rva:");
                        for (USHORT i = 0; i < got; i++) {
                            HMODULE m = NULL;
                            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               (LPCSTR)fr[i], &m);
                            if ((char*)m == mb)
                                fprintf(stderr, " %llX", (unsigned long long)((char*)fr[i] - mb));
                        }
                        fprintf(stderr, "\n");
                        if (g_active_ctx)
                            fprintf(stderr, "[VM-SWEEP]   guest cia=0x%08X lr=0x%08X r3=0x%08X r4=0x%08X "
                                            "r5=0x%08X r6=0x%08X r9=0x%08X r10=0x%08X r11=0x%08X\n",
                                    (uint32_t)g_active_ctx->cia, (uint32_t)g_active_ctx->lr,
                                    (uint32_t)g_active_ctx->gpr[3], (uint32_t)g_active_ctx->gpr[4],
                                    (uint32_t)g_active_ctx->gpr[5], (uint32_t)g_active_ctx->gpr[6],
                                    (uint32_t)g_active_ctx->gpr[9], (uint32_t)g_active_ctx->gpr[10],
                                    (uint32_t)g_active_ctx->gpr[11]);
                        fflush(stderr);
                    }
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: %s <EBOOT.elf>\n", argv[0]); return 2; }

#ifdef _WIN32
    SetUnhandledExceptionFilter(ydkj_crash_filter);
    signal(SIGABRT, ydkj_abort_handler);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: don't lose prints on kill */
#endif

    /* Flat VM: one host buffer, guest addr -> vm_base + addr. This maps the
     * FULL 32-bit guest space uniformly (page 0, the 0x60000000..0xD0000000
     * range, everything) -- which native-VA mapping can't on Windows, because
     * the OS reserves the low 64 KB and DLLs occupy parts of the mid range.
     * On real PS3 those addresses are RAM, and the game writes to them (its
     * null-object inits land on page 0); calloc backs them so the game runs.
     * HLE functions that take guest pointers must translate via vm_base /
     * vm_write* (which also byte-swap) -- a raw *guest_ptr would deref the host
     * buffer's offset incorrectly. */
#ifdef _WIN32
    /* Reserve the full 4 GB guest space; pages commit on first touch via the VEH. */
    /* Silent-death catchers (ported from the YDKJ harness after the Bink
     * bring-up: stack overflow + guard-page violations kill the process with
     * NO output from the standard crash filter). */
    AddVectoredExceptionHandler(0 /*last*/, [](EXCEPTION_POINTERS* ep)->LONG{
        DWORD c = ep->ExceptionRecord->ExceptionCode;
        if (c == 0xC00000FDu /*STACK_OVERFLOW*/ || c == 0x80000001u /*GUARD_PAGE*/) {
            fprintf(stderr,"\n[FATAL-%08lX] tid=%lu fault=0x%llX rip=%p; bt RVAs:",
                    (unsigned long)c, GetCurrentThreadId(),
                    ep->ExceptionRecord->NumberParameters >= 2 ?
                        (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1] : 0ull,
                    ep->ExceptionRecord->ExceptionAddress);
            HMODULE mod = GetModuleHandleA(0); void* fr[32];
            USHORT n = RtlCaptureStackBackTrace(0, 32, fr, 0);
            for (USHORT i = 0; i < n; i++)
                fprintf(stderr, " %llX", (unsigned long long)((char*)fr[i] - (char*)mod));
            fprintf(stderr, "\n"); fflush(stderr);
            if (c == 0xC00000FDu) ExitProcess(7);
        }
        return EXCEPTION_CONTINUE_SEARCH; });
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;   /* full 32-bit space backed -> OOB guard unnecessary */
#else
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
#endif
    if (!vm_base) { printf("vm alloc failed\n"); return 1; }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) { printf("load failed\n"); return 1; }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    /* Real title id (BCUS98148) from the disc PARAM.SFO -- otherwise every
     * title-id path (cellGameContentPermit, /dev_hdd1 cache, hash.dat) uses
     * the BLES00000 placeholder and update-data lookups miss. */
    { char sfo[1100]; snprintf(sfo, sizeof sfo, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
      cellGame_init_from_paramsfo(sfo); }

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* real system PRX (libsre) -> guest RAM + exports */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT (sys_initialize_tls, ...) */
    ppu_fs_register();       /* cellFs VFS over the real game directory */
    lv2_init_syscalls();     /* real lv2 syscall table (semaphore/memory/fs/...) */
    /* cellSaveDataUser* take NINE args; the generic adapter only forwards
     * r3..r10, so the ninth (userdata) needs the full context. Registered after
     * ppu_hle_init so the ctx handler overrides the generated 8-arg entry. */
    cellSaveData_register_ctx_handlers();

    /* The wwsjob policy module the game registers via cellSpursAddWorkload is a
     * RAW 11520-byte blob at EA 0x848700 (all 14 Wws_Job workloads share it).
     * The generated registration (gen/spu_workloads.c) fingerprints the
     * ELF-wrapped lift artifact; alias the RAW bytes' fingerprint — the ones
     * cellSpurs actually hashes at kick time — to the same lifted entry.
     * (Decls at file scope above main — extern "C" is illegal at block scope.)
     *
     * NB: the image ids below MUST match the ones gen/spu_workloads.c assigns,
     * which it does in sorted-filename order -- so adding an image renumbers
     * every image after it. Check gen/spu_workloads.c after any re-generation.
     * (job_wws_7BD900 sorting before pm_wwsjob is exactly what shifted the PM
     * from image 1 to image 2.) */
    spu_workload_register_img(0x1755B9CC1F95E680ULL,
            pm_wwsjob_spu_func_00000A00, 2, "pm_wwsjob(raw)");
    /* The jobchain's job binary: a raw WWS JOBCRT blob at 0x7BD900 (190752 B).
     * The generated table registers the ELF-wrapped lift; alias the RAW bytes'
     * fingerprint -- the ones the chain's CellSpursJobHeader points at, which is
     * what dispatch actually hashes -- onto the same lifted entry and image. */
    spu_workload_register_img(0x080AFC0306F87CE0ULL,
            job_wws_7BD900_spu_func_00000000, 1, "job_wws_7BD900(raw)");

    /* Install the guest-callback hook and start the synthetic RSX vblank driver
     * so the game's frame loop advances (it no-ops until the game registers its
     * vblank/flip handlers during init). */
    g_ps3_guest_caller = harness_guest_caller;
#ifdef _WIN32
    CreateThread(NULL, 4u * 1024 * 1024, vblank_ticker, NULL, 0, NULL);
    CreateThread(NULL, 0, hang_watchdog, NULL, 0, NULL);
    if (getenv("PS3_GUEST_PROF"))
        CreateThread(NULL, 0, guest_prof_thread, NULL, 0, NULL);
    if (getenv("UNSTICK")) CreateThread(NULL, 0, unstick_thread, NULL, 0, NULL);
#endif

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n", entry, STACK_TOP);
    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);
    return 0;
}

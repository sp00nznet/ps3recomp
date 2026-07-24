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
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
extern const char* ppu_vfs_root;   /* host dir that PS3 mount points map into */
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
    fprintf(stderr, "[CRASH] module=%p rva=0x%llX  (llvm-symbolizer --obj=ydkj_boot.exe 0x%llX)\n",
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
extern "C" int  cellGcm_take_flip_pending(void);  /* hoisted to file scope: clang-cl rejects block-scope extern "C" */

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{ ppu_guest_call(opd, a0, a1, a2, a3); }

#ifdef _WIN32
/* RSX present backend (libs/video/rsx_d3d12_backend.c). Driven on the vblank
 * thread so the D3D12 device + window message pump live on one thread. */
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int cellGcm_take_flip_pending(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" void cellGcm_rsx_process_fifo(void);   /* cellGcmSys.c: drain get->put */
extern "C" unsigned cellGcm_flip_request_count(void);
extern "C" int sys_event_queue_inject(unsigned int, unsigned long long, unsigned long long, unsigned long long, unsigned long long);

/* ---- Guest-PC sampling profiler (PS3_GUEST_PROF=1) -----------------------
 * Samples every guest thread's ctx.cia every ~5ms and dumps the top sites
 * every 5s. cia is refreshed at every syscall, and guest spin loops issue a
 * syscall per iteration (yield/usleep), so a thread stuck in a wait loop
 * shows its loop's PC dominating the histogram. This is the tool that
 * answers "where does thread X spend its wall time" without a debugger. */
#include "../../syscalls/sys_ppu_thread.h"
static DWORD WINAPI guest_prof_thread(LPVOID)
{
    struct Site { uint32_t tid, cia; uint32_t hits; };
    static Site sites[256];
    int nsites = 0;
    ULONGLONG t0 = GetTickCount64();
    unsigned total = 0;
    for (;;) {
        Sleep(5);
        for (int i = 0; i < PPU_THREAD_MAX; i++) {
            ppu_thread_info* t = &g_ppu_threads[i];
            if (t->state != 1 /*RUNNING-ish; harmless if enum differs*/ && t->state != 2) continue;
            uint32_t cia = (uint32_t)t->ctx.cia;
            if (!cia) continue;
            uint32_t tid = (uint32_t)(i + 1);
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
                        g_ppu_threads[sites[best].tid - 1].name);
                sites[best].hits = 0;   /* consume */
            }
            fflush(stderr);
            nsites = 0; total = 0; t0 = now;
        }
    }
}

static DWORD WINAPI vblank_ticker(LPVOID)
{
    const char* _title = getenv("PS3_TITLE");
    if (!_title || !*_title) _title = "You Don't Know Jack (ps3recomp)";
    int rsx_ok = (rsx_d3d12_backend_init(1280, 720, _title) == 0);
    fprintf(stderr, "[rsx] backend init %s\n", rsx_ok ? "OK -- window open" : "FAILED");
    unsigned last_flip = 0;
    /* The game's frame pacing (vblank/flip handlers -> display frame counter) must
     * advance at ~60Hz regardless of how long present() blocks. On a hidden/occluded
     * window DXGI Present throttles hard, which previously stalled these ticks and
     * paced the game's main loop to ~0.5fps. Drive the ticks off REAL elapsed time
     * and catch up in a bounded burst so present latency never slows the game. */
    ULONGLONG next_tick = GetTickCount64();
    for (;;) {
        Sleep(4);
        ULONGLONG now = GetTickCount64();
        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining further: the flip fires
             * at a get==put frame boundary (guest thread), so the batch held
             * right now is exactly the completed frame. Presenting on a raw
             * flip-count change after the drain raced the guest's next-frame
             * writes and showed empty or mixed batches. */
            {
                if (rsx_ok && cellGcm_take_flip_pending()) {
                    rsx_d3d12_backend_present();
                    last_flip = cellGcm_flip_request_count();
                }
            }
            /* Drain the game's GCM FIFO every tick -- this writes the RSX sync-fence
             * labels (e.g. @0x03000410) that the game's per-frame logic blocks on.
             * Doing it here (not after present) keeps those fences advancing at 60Hz
             * even when present() throttles on a hidden/occluded window. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += 16;                 /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -> resync */
        /* Drain at the outer ~4ms cadence too (not just the 16ms tick):
         * titles fence EVERY render pass on an RSX label the drain writes;
         * at 16ms per fence wave's six passes paced the guest to ~7 fps.
         * The real RSX writes those fences in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                rsx_d3d12_backend_present();
                last_flip = cellGcm_flip_request_count();
            }
            cellGcm_rsx_process_fifo();
        }
        /* YDKJ_INJECT_Q3: the main thread polls q3 (load/state-complete) each frame but
         * it's never posted (producer signals a condvar, never enqueues). Inject a q3
         * event periodically to test whether delivering the completion advances the
         * game's state machine to instantiate + display the (force-parsed) menu movie. */
        if (getenv("YDKJ_INJECT_Q3")) {
            static int s_q3=0; const char* qe=getenv("YDKJ_INJECT_Q3"); uint32_t qid=(uint32_t)strtoul(qe,0,0); if(qid==0||qid==1) qid=3;
            if(++s_q3 % 8 == 0){ int r=sys_event_queue_inject(qid, 0x1234, 0, 0, 0);
              static int _n=0; if(_n++<12) fprintf(stderr,"[INJQ3] injected q%u event rc=%d\n",qid,r); } }
        if (rsx_ok) {
            if (rsx_d3d12_backend_pump_messages() != 0) { rsx_ok = 0; }
            if (getenv("YDKJ_PACETRACE")) {
                static ULONGLONG s_win=0; static int s_pf=0, s_pres=0; static ULONGLONG s_presms=0;
                s_pf += fired; s_pres++;
                ULONGLONG t0=GetTickCount64(); rsx_d3d12_backend_present(); ULONGLONG t1=GetTickCount64();
                s_presms += (t1-t0);
                if (s_win==0) s_win=now;
                if (now - s_win >= 1000) {
                    fprintf(stderr,"[PACE] process_fifo=%d/s  present=%d/s  present_total=%llums/s (avg %llums)\n",
                            s_pf, s_pres, s_presms, s_pres? s_presms/s_pres:0);
                    s_pf=0; s_pres=0; s_presms=0; s_win=now;
                }
            } else {
                /* Present only on a guest flip (frame boundary). A fixed-clock
                 * present can catch the drain mid-frame -- notably while the
                 * guest is blocked in the FIFO-wrap recycle callback -- and
                 * flash a partial frame (clear + a few draws). Before the
                 * first flip present freely so the window isn't stuck white
                 * during boot. */
                unsigned fc = cellGcm_flip_request_count();
                if (fc != last_flip || fc == 0) {
                    rsx_d3d12_backend_present();
                    last_flip = fc;
                }
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
static void dump_threads(const char* label, HMODULE self)
{
    fprintf(stderr, "[WATCHDOG] %s; last HLE call = 0x%08X (%s)\n",
            label, g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
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
                } else {
                    char path[MAX_PATH] = "?";
                    if (m) GetModuleFileNameA(m, path, sizeof path);
                    const char* base = strrchr(path, '\\');
                    fprintf(stderr, "[WATCHDOG]   tid %5lu in %s\n",
                            (unsigned long)te.th32ThreadID, base ? base + 1 : path);
                }
                /* Scan the suspended thread's stack for boot-module return
                 * addresses to reconstruct the lifted call chain — done for ALL
                 * threads (even when RIP is parked in ntdll inside a CriticalSection
                 * call), since that's exactly where the busy-spin's lwmutex churn
                 * lands the main thread. Map RVAs -> func_ names via flow.map.
                 * (some false positives expected — these are stack-scan hits.) */
                {
                    uint64_t* sp = (uint64_t*)ctx.Rsp;
                    /* Bound the scan to the committed stack region so we never read
                     * past the guard page. If the query fails or the region is not
                     * committed+readable (thread mid-create, guard page, garbage
                     * Rsp), skip the scan entirely -- a diagnostic AV here gets
                     * caught by the crash filter and kills the whole run. */
                    MEMORY_BASIC_INFORMATION mbi;
                    uint64_t region_end = (uint64_t)sp;
                    if (VirtualQuery((LPCVOID)sp, &mbi, sizeof mbi) &&
                        mbi.State == MEM_COMMIT &&
                        !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                                        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                        region_end = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
                    int maxk = (int)((region_end - (uint64_t)sp) / 8);
                    if (maxk > 0x20000 / 8) maxk = 0x20000 / 8;
                    int found = 0;
                    for (int k = 0; k < maxk && found < 20; k++) {
                        uint64_t v = sp[k];
                        if (v < (uint64_t)self) continue;
                        HMODULE mm = NULL;
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCSTR)v, &mm);
                        if (mm == self) {
                            fprintf(stderr, "[WATCHDOG]       tid %5lu ret rva=0x%llX\n",
                                    (unsigned long)te.th32ThreadID,
                                    (unsigned long long)(v - (uint64_t)self));
                            found++;
                        }
                    }
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
    return 0;
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
/* Guard-page-violation reporter: STATUS_GUARD_PAGE_VIOLATION (0x80000001)
 * bypasses every ACCESS_VIOLATION-only handler and kills the process SILENTLY
 * (observed the moment the Bink SPU decoder first really ran). Log full context
 * before letting the chain continue. */
static LONG WINAPI guard_report_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == 0x80000001u /*STATUS_GUARD_PAGE_VIOLATION*/) {
        static LONG s_n = 0;
        if (InterlockedIncrement(&s_n) <= 8) {
            char* mbase = (char*)GetModuleHandleA(NULL);
            fprintf(stderr, "\n[GUARDVIOLATION] tid=%lu %s fault=0x%llX rip-rva=0x%llX\n",
                    GetCurrentThreadId(),
                    ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                    (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
                    (unsigned long long)((char*)ep->ExceptionRecord->ExceptionAddress - mbase));
            void* fr[24]; USHORT n = RtlCaptureStackBackTrace(0, 24, fr, NULL);
            fprintf(stderr, "[GUARDVIOLATION] bt-rva:");
            for (USHORT i = 0; i < n; i++)
                fprintf(stderr, " %llX", (unsigned long long)((char*)fr[i] - mbase));
            fprintf(stderr, "\n"); fflush(stderr);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: %s <EBOOT.elf>\n", argv[0]); return 2; }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    timeBeginPeriod(1);   /* 1ms timer resolution: the default ~15.6ms granularity
                           * inflates every sub-15ms wait (the game's event polls,
                           * usleeps) and throttled the whole title. */
    SetUnhandledExceptionFilter(ydkj_crash_filter);
    AddVectoredExceptionHandler(0 /*last*/, [](EXCEPTION_POINTERS* ep)->LONG{
        if (ep->ExceptionRecord->ExceptionCode == 0xC00000FDu /*STACK_OVERFLOW*/) {
            fprintf(stderr,"\n[STACKOVERFLOW] infinite recursion detected; backtrace (RVAs):\n");
            HMODULE mod=GetModuleHandleA(0); void* fr[62]; USHORT n=RtlCaptureStackBackTrace(0,62,fr,0);
            for(USHORT i=0;i<n;i++) fprintf(stderr," %llX",(unsigned long long)((char*)fr[i]-(char*)mod));
            fprintf(stderr,"\n"); fflush(stderr); ExitProcess(7);
        }
        return EXCEPTION_CONTINUE_SEARCH; });
    { ULONG g=256*1024; SetThreadStackGuarantee(&g); }  /* reserve stack so the SO handler can run */
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
    AddVectoredExceptionHandler(1, guard_report_veh);  /* diagnose 0x80000001 silent deaths */
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

    fprintf(stderr,"[boot-dbg] before ppu_recomp_register\n"); fflush(stderr);
    ppu_recomp_register();   /* lifted function table -> address map */
    fprintf(stderr,"[boot-dbg] after ppu_recomp_register; before ps3_load_prx_modules\n"); fflush(stderr);
    ps3_load_prx_modules();  /* real system PRX (libsre) -> guest RAM + exports */
    fprintf(stderr,"[boot-dbg] after prx; before ppu_hle_init\n"); fflush(stderr);
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT (sys_initialize_tls, ...) */
    ppu_fs_register();       /* cellFs VFS over the real game directory */
    fprintf(stderr,"[boot-dbg] before lv2_init_syscalls\n"); fflush(stderr);
    lv2_init_syscalls();     /* real lv2 syscall table (semaphore/memory/fs/...) */
    fprintf(stderr,"[boot-dbg] after lv2_init_syscalls\n"); fflush(stderr);

    /* Install the guest-callback hook and start the synthetic RSX vblank driver
     * so the game's frame loop advances (it no-ops until the game registers its
     * vblank/flip handlers during init). */
    g_ps3_guest_caller = harness_guest_caller;
#ifdef _WIN32
    CreateThread(NULL, 4u * 1024 * 1024, vblank_ticker, NULL, 0, NULL);
    CreateThread(NULL, 0, hang_watchdog, NULL, 0, NULL);
    if (getenv("PS3_GUEST_PROF"))
        CreateThread(NULL, 0, guest_prof_thread, NULL, 0, NULL);
#endif

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n", entry, STACK_TOP);
#ifdef _WIN32
    fprintf(stderr, "[boot] MAIN guest thread tid=%lu\n", (unsigned long)GetCurrentThreadId());
#endif
    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);
    return 0;
}

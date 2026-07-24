/*
 * ps3recomp - lifter torture-test harness (host side).
 *
 * Boots a self-checking guest ELF (tests/guest/*.c compiled with the real
 * PS3 SDK ppu-lv2-gcc) through the full pipeline: loader + lifted code + HLE
 * bridge + lv2 syscalls. The guest prints one line per test:
 *
 *     ok <n> <name>
 *     FAIL <name> got=<hex> want=<hex>
 *     TORTURE COMPLETE pass=<n> fail=<n>
 *
 * tests/run_torture.py parses that output. This binary's job is only to boot
 * the guest and make every failure mode LOUD:
 *   - crash        -> [CRASH] + module RVA (llvm-symbolizer against the .map)
 *   - guest hang   -> [TIMEOUT] after $TORTURE_TIMEOUT seconds (default 30),
 *                     thread RIP dump, exit code 2 (a hang IS a test failure:
 *                     e.g. the newlib dtoa infinite loop class of lifter bug)
 *   - clean finish -> exit 0; the text output carries pass/fail counts
 */
#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

extern "C" {
uint32_t ppu_load_elf(const char* path);
void     ppu_recomp_register(void);
void     ppu_hle_init(void);
void     ppu_sysprx_register(void);
void     ppu_fs_register(void);
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
extern const char* ppu_vfs_root;
void     ps3_load_prx_modules(void) __attribute__((weak));
void     ps3_load_prx_modules(void) {}
}

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

extern "C" uint32_t    g_last_hle_nid;    /* ppu_hle.cpp breadcrumb */
extern "C" const char* g_last_hle_name;
extern "C" __declspec(thread) ppu_context* g_active_ctx;

static LONG WINAPI torture_crash_filter(EXCEPTION_POINTERS* ep)
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
    if (g_active_ctx)
        fprintf(stderr, "[CRASH] guest ctr=0x%08X lr=0x%08X r1=0x%08X r3=0x%08X\n",
                (uint32_t)g_active_ctx->ctr, (uint32_t)g_active_ctx->lr,
                (uint32_t)g_active_ctx->gpr[1], (uint32_t)g_active_ctx->gpr[3]);
    HMODULE mod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)er->ExceptionAddress, &mod);
    fprintf(stderr, "[CRASH] module=%p rva=0x%llX  (llvm-symbolizer --obj=torture.exe 0x%llX)\n",
            (void*)mod, (unsigned long long)((char*)er->ExceptionAddress - (char*)mod),
            (unsigned long long)((char*)er->ExceptionAddress - (char*)mod));
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
    _exit(3);
}

/* Hang watchdog: a guest that stops making progress is a failed test, not a
 * reason for the harness to sit forever. Dump where every thread is parked
 * (module RVA for lifted code) then hard-exit 2. */
static DWORD WINAPI torture_watchdog(LPVOID)
{
    int secs = 30;
    const char* env = getenv("TORTURE_TIMEOUT");
    if (env && atoi(env) > 0) secs = atoi(env);
    Sleep((DWORD)secs * 1000);

    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&torture_watchdog, &self);
    fprintf(stderr, "\n[TIMEOUT] guest still running after %d s; last HLE 0x%08X (%s)\n",
            secs, g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
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
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &c)) {
                HMODULE m = NULL;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)c.Rip, &m);
                if (m == self) {
                    fprintf(stderr, "[TIMEOUT]   tid %5lu rip rva=0x%llX",
                            (unsigned long)te.th32ThreadID,
                            (unsigned long long)((char*)c.Rip - (char*)self));
                    /* stack-scan a few return RVAs for the call chain */
                    uint64_t* sp = (uint64_t*)c.Rsp;
                    int found = 0;
                    fprintf(stderr, "  ret:");
                    for (int k = 0; k < 0x4000 / 8 && found < 8; k++) {
                        HMODULE mm = NULL;
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCSTR)sp[k], &mm);
                        if (mm == self) {
                            fprintf(stderr, " 0x%llX",
                                    (unsigned long long)(sp[k] - (uint64_t)self));
                            found++;
                        }
                    }
                    fprintf(stderr, "\n");
                } else {
                    char path[MAX_PATH] = "?";
                    if (m) GetModuleFileNameA(m, path, sizeof path);
                    const char* base = strrchr(path, '\\');
                    fprintf(stderr, "[TIMEOUT]   tid %5lu in %s\n",
                            (unsigned long)te.th32ThreadID, base ? base + 1 : path);
                }
            }
            ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    fflush(stderr);
    _exit(2);
}

#define VM_SIZE    0x100010000ull /* full 32-bit guest space + guard, demand-committed */

static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep);
#endif

#define STACK_TOP  0x0FF00000u

/* Host-provided symbols the runtime + HLE libs need. */
extern "C" uint8_t* vm_base = nullptr;
extern "C" uint32_t ppu_vm_size;
extern "C" void lv2_init_syscalls(void);

typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" ps3_guest_caller_fn g_ps3_guest_caller;
extern "C" uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t);
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{ ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7); }

#ifdef _WIN32
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
    if (argc < 2) { printf("usage: %s <guest.elf>\n", argv[0]); return 64; }

#ifdef _WIN32
    SetUnhandledExceptionFilter(torture_crash_filter);
    setvbuf(stdout, NULL, _IONBF, 0);
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;
#else
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
#endif
    if (!vm_base) { printf("vm alloc failed\n"); return 1; }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) { printf("load failed\n"); return 1; }

    ppu_vfs_root = ".";

    ppu_recomp_register();
    ps3_load_prx_modules();
    ppu_hle_init();
    ppu_sysprx_register();
    ppu_fs_register();
    lv2_init_syscalls();

    g_ps3_guest_caller = harness_guest_caller;
#ifdef _WIN32
    CreateThread(NULL, 0, torture_watchdog, NULL, 0, NULL);
#endif

    fprintf(stderr, "[torture] dispatching entry OPD 0x%08X\n", entry);
    int rc = ppu_run(entry, STACK_TOP);
    fprintf(stderr, "[torture] ppu_run returned %d\n", rc);
    return 0;
}

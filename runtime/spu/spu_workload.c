/* spu_workload.c — SPU workload / task dispatch registry (see spu_workload.h).
 *
 * Maps a registered SPU image (by FNV-1a-64 content fingerprint) to its
 * pre-lifted native entry, loads the image into a 256 KB local store, and runs
 * it with the SPURS task ABI. cellSpurs's AddWorkload/CreateTask call
 * spu_workload_dispatch(); the registry is populated by the title's lifted set.
 */
#include "spu_workload.h"
#include "spu_lifted_job.h"   /* spu_run_lifted_job */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* Set by the MFC DMA engine (spu_dma.h) when the cri task (image 22) issues a
 * real video-payload GET (>256B from a non-context EA) = it actually decoded,
 * as opposed to the 64-byte context handshake DMAs. The dispatcher's
 * YDKJ_CRI_RESUME poll-loop watches this to know real work arrived. */
int g_cri_video_dma = 0;

/* Game-provided lifted SPU symbol used only by the ydkj CRI-taskset diagnostic
 * path below (image 22 + YDKJ_CRI_TASKSET). Weak default so titles that don't
 * ship that SPU image still link; a game that lifts it supplies the strong def.
 *
 * MSVC has no __attribute__((weak)) and the runtime lib builds under MSVC (the
 * per-game exe is clang-cl). Under MSVC we simply omit the default, which is
 * exactly the pre-existing behaviour -- a title that doesn't ship the image gets
 * an unresolved external, same as before this default was added. */
#if defined(__clang__) || defined(__GNUC__)
__attribute__((weak)) void tsp_spu_func_00000A00(spu_context* c) { (void)c; }
#endif

/* ---- fingerprint ------------------------------------------------------- */

uint64_t spu_workload_fingerprint(const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ULL;          /* FNV offset basis */
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;                     /* FNV prime */
    }
    return h;
}

/* ---- registry ---------------------------------------------------------- */

#ifndef SPU_WORKLOAD_MAX
#define SPU_WORKLOAD_MAX 256
#endif

typedef struct {
    uint64_t            fp;
    spu_lifted_entry_fn fn;
    int                 image_id;
    const char*         name;
} spu_workload_entry;

static spu_workload_entry s_registry[SPU_WORKLOAD_MAX];
static unsigned           s_registry_count = 0;

void spu_workload_register_img(uint64_t fingerprint, spu_lifted_entry_fn fn,
                               int image_id, const char* name)
{
    if (!fn) return;
    for (unsigned i = 0; i < s_registry_count; i++) {
        if (s_registry[i].fp == fingerprint) {     /* idempotent on fingerprint */
            s_registry[i].fn       = fn;
            s_registry[i].image_id = image_id;
            s_registry[i].name     = name;
            return;
        }
    }
    if (s_registry_count >= SPU_WORKLOAD_MAX) {
        fprintf(stderr, "[spu_workload] registry full (%u); dropping '%s'\n",
                SPU_WORKLOAD_MAX, name ? name : "?");
        return;
    }
    s_registry[s_registry_count].fp       = fingerprint;
    s_registry[s_registry_count].fn       = fn;
    s_registry[s_registry_count].image_id = image_id;
    s_registry[s_registry_count].name     = name;
    s_registry_count++;
}

void spu_workload_register(uint64_t fingerprint, spu_lifted_entry_fn fn,
                           const char* name)
{
    spu_workload_register_img(fingerprint, fn, 0, name);
}

spu_lifted_entry_fn spu_workload_find(uint64_t fingerprint)
{
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fingerprint)
            return s_registry[i].fn;
    return NULL;
}

unsigned spu_workload_count(void) { return s_registry_count; }

/* ---- SPU ELF loader (32-bit big-endian) -------------------------------- */

static uint16_t rd_be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

int spu_elf_load_to_ls(const uint8_t* image, size_t image_size, uint8_t* ls, uint32_t* entry_out)
{
    if (!image || !ls || image_size < 0x34) return 0;

    /* ELF ident: 0x7F 'E' 'L' 'F', ELFCLASS32 (1), ELFDATA2MSB (2). */
    if (!(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F'))
        return 0;
    if (image[4] != 1 /*ELFCLASS32*/ || image[5] != 2 /*ELFDATA2MSB*/)
        return 0;

    uint32_t e_entry     = rd_be32(image + 0x18);
    uint32_t e_phoff     = rd_be32(image + 0x1C);
    uint16_t e_phentsize = rd_be16(image + 0x2A);
    uint16_t e_phnum     = rd_be16(image + 0x2C);
    if (e_phentsize < 0x20) e_phentsize = 0x20;

    for (uint16_t i = 0; i < e_phnum; i++) {
        size_t po = (size_t)e_phoff + (size_t)i * e_phentsize;
        if (po + 0x20 > image_size) break;
        const uint8_t* ph = image + po;

        uint32_t p_type   = rd_be32(ph + 0x00);
        if (p_type != 1 /*PT_LOAD*/) continue;
        uint32_t p_offset = rd_be32(ph + 0x04);
        uint32_t p_vaddr  = rd_be32(ph + 0x08);
        uint32_t p_filesz = rd_be32(ph + 0x10);
        uint32_t p_memsz  = rd_be32(ph + 0x14);

        /* bounds: segment must fit in local store and in the image */
        if ((uint64_t)p_vaddr + p_memsz > SPU_LS_SIZE)            return 0;
        if ((uint64_t)p_offset + p_filesz > image_size)          return 0;

        if (p_filesz) memcpy(ls + p_vaddr, image + p_offset, p_filesz);
        if (p_memsz > p_filesz)
            memset(ls + p_vaddr + p_filesz, 0, p_memsz - p_filesz);
    }
    if (entry_out) *entry_out = e_entry;
    return 1;
}

size_t spu_elf_image_size(const uint8_t* image, size_t max_avail)
{
    if (!image || max_avail < 0x34) return 0;
    if (!(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F'))
        return 0;
    if (image[4] != 1 || image[5] != 2) return 0;     /* ELFCLASS32, ELFDATA2MSB */

    uint32_t e_phoff     = rd_be32(image + 0x1C);
    uint32_t e_shoff     = rd_be32(image + 0x20);
    uint16_t e_phentsize = rd_be16(image + 0x2A);
    uint16_t e_phnum     = rd_be16(image + 0x2C);
    uint16_t e_shentsize = rd_be16(image + 0x2E);
    uint16_t e_shnum     = rd_be16(image + 0x30);

    uint64_t end = (uint64_t)e_shoff + (uint64_t)e_shnum * e_shentsize;

    for (uint16_t k = 0; k < e_phnum; k++) {           /* program headers */
        size_t po = (size_t)e_phoff + (size_t)k * e_phentsize;
        if (po + 0x14 > max_avail) break;
        uint32_t p_offset = rd_be32(image + po + 0x04);
        uint32_t p_filesz = rd_be32(image + po + 0x10);
        uint64_t e = (uint64_t)p_offset + p_filesz;
        if (e > end) end = e;
    }
    for (uint16_t k = 0; k < e_shnum; k++) {           /* section headers */
        size_t so = (size_t)e_shoff + (size_t)k * e_shentsize;
        if (so + 0x18 > max_avail) break;
        uint32_t sh_type   = rd_be32(image + so + 0x04);
        uint32_t sh_offset = rd_be32(image + so + 0x10);
        uint32_t sh_size   = rd_be32(image + so + 0x14);
        if (sh_type != 8 /*SHT_NOBITS*/) {
            uint64_t e = (uint64_t)sh_offset + sh_size;
            if (e > end) end = e;
        }
    }
    if (end > max_avail) end = max_avail;
    return (size_t)end;
}

/* ---- dispatch ---------------------------------------------------------- */

int spu_workload_dispatch(const uint8_t* image, uint32_t image_size,
                          uint32_t args_ea)
{
    if (!image || image_size == 0) return 0;

    uint64_t fp = spu_workload_fingerprint(image, image_size);
    spu_lifted_entry_fn fn = NULL;
    int image_id = 0;
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fp) { fn = s_registry[i].fn; image_id = s_registry[i].image_id; break; }
    if (!fn) {
        fprintf(stderr,
            "[spu_workload] dispatch MISS fp=0x%016llX size=%u "
            "(no lifted SPU binary registered for this image)\n",
            (unsigned long long)fp, image_size);
        return 0;
    }

    /* Load the SPU ELF into a fresh local store, then run the lifted entry with
     * the task arg in r3. 256 KB is heap-allocated (too large for the stack,
     * and spu_run_lifted_job already builds a full spu_context on its stack). */
    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (!ls) return 0;

    uint32_t entry = 0;
    if (!spu_elf_load_to_ls(image, image_size, ls, &entry)) {
        fprintf(stderr, "[spu_workload] dispatch fp=0x%016llX: not a valid SPU ELF\n", (unsigned long long)fp);
        free(ls);
        return 0;
    }

    fprintf(stderr,
        "[spu_workload] dispatch HIT fp=0x%016llX entry=0x%05X args=0x%08X image=%d -> running\n",
        (unsigned long long)fp, entry, args_ea, image_id);

    spu_run_lifted_job_img(fn, ls, args_ea, image_id);

    free(ls);
    return 1;
}

/* Async dispatch: run the SPU job on its OWN host thread so the PPU caller is
 * not blocked. SPURS service/worker tasks are persistent — they loop waiting on
 * PPU-side signals (DMA, event flags, event queues), so running them inline (as
 * spu_workload_dispatch does) deadlocks: the PPU can never deliver the signal
 * the SPU is waiting for because it is stuck inside the dispatch. Real SPUs run
 * concurrently with the PPU; a detached host thread models that. The image bytes
 * and args live in the shared guest arena (vm_base), so they stay valid. */
typedef struct {
    const uint8_t*      image;
    uint32_t            image_size;
    uint32_t            args_ea;
    spu_lifted_entry_fn fn;
    int                 image_id;
    uint32_t            r3[4];        /* captured race-free at dispatch time */
    int                 have_r3;
    uint32_t            taskset_ea;   /* captured race-free at dispatch (globals get clobbered) */
    uint32_t            taskid;
} spu_async_job;

static void spu_async_run(spu_async_job* j)
{
    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (ls) {
        uint32_t entry = 0;
        int loaded = spu_elf_load_to_ls(j->image, j->image_size, ls, &entry);
        if (!loaded)
            fprintf(stderr, "[spu_workload] async image=%d ELF LOAD FAILED\n",
                    j->image_id), fflush(stderr);
        if (loaded) {
            /* YDKJ_CRI_POLICY (cri build experiment): the cri SPU task
             * (image 22) calls the SPURS task-API via a jump table at LS 0x2700
             * and reads its task descriptor at LS 0x2FB0 — both POLICY-provided,
             * not in the task ELF (so it branch-to-0's at func_00026DE0). Preload
             * the policy module (libsre @0x30021480 -> LS 0xA00) and write the
             * task descriptor {0xFFFFFFFF, 0x400, 0x2700, 0x3000} at 0x2FB0, then
             * run the cri task, to see how much further it gets. Diagnostic only. */
            if (j->image_id == 22 && getenv("YDKJ_CRI_TASKSET")) {
                /* cri build: load the TASKSET POLICY module (libsre 0x23680 ->
                 * LS 0xA00) alongside the cri task (already at 0x3000), then RUN
                 * THE POLICY ENTRY (tsp_spu_func_00000A00) under image 23. The
                 * policy builds the 0x2700 task-API table + dispatches the cri
                 * task. Needs the SpursKernelContext at LS[0x1C0] (instance ptr)
                 * + r80=0x100 context base, mirroring the kernel->policy handoff. */
                extern uint8_t* vm_base;
                extern void tsp_spu_func_00000A00(spu_context*);
                if (vm_base) memcpy(ls + 0xA00, vm_base + 0x30023680, 0x2200);
                /* Build the REAL SpursTasksetContext at LS 0x2700 (taskset ptr @0x27B8,
                 * TaskInfo @0x2780, syscallAddr @0x27C4) from the actual game taskset,
                 * so the policy's DMAs read valid data instead of garbage. The instance
                 * ptr (LS[0x1C0]) was a hardcoded 0x40009D00 that mismatched the game's
                 * real 0x40009F00 -> policy DMA'd garbage -> null branches. */
                extern uint64_t spurs_pm_build_context(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t);
                extern uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
                if (g_ydkj_real_taskset_ea)
                    spurs_pm_build_context(ls, g_ydkj_real_taskset_ea, g_ydkj_real_taskid, 0, 0);
                uint32_t inst = g_ydkj_real_spurs_ea ? g_ydkj_real_spurs_ea : 0x40009D00u;
                uint8_t* p = ls + 0x1C0;
                p[0]=0; p[1]=0; p[2]=0; p[3]=0;                 /* hi32 of u64 */
                p[4]=(uint8_t)(inst>>24); p[5]=(uint8_t)(inst>>16);
                p[6]=(uint8_t)(inst>>8);  p[7]=(uint8_t)inst;   /* lo32 */
                fprintf(stderr, "[cri] YDKJ_CRI_TASKSET: loaded taskset policy@0xA00, LS[0x1C0]=inst, running policy entry (img23)\n");
                fflush(stderr);
                /* Run the taskset policy entry instead of the cri task entry.
                 * (spu_run_lifted_job_abi is declared in spu_lifted_job.h.) */
                int32_t prc = spu_run_lifted_job_abi(tsp_spu_func_00000A00, ls,
                                                     j->args_ea, 23, 1, j->have_r3 ? j->r3 : 0);
                fprintf(stderr, "[cri] taskset policy RETURNED rc=%d\n", prc);
                fflush(stderr);
                free(ls); free(j); return;
            }
            /* YDKJ HLE cri-task path (image 22, cri_mpvps3spurs.elf): the real
             * SPURS kernel/policy would build the task's SpursTasksetContext (LS
             * 0x2700) before dispatch. In the HLE-cellSpurs path we dispatch the
             * task ELF directly, so LS 0x2700 is empty and the task branch-to-0's:
             * cri func_00026DE0 reads syscallAddr@0x27C4 (=0) and branches there.
             * Plant a minimal context so the task-API call lands on the HLE syscall
             * trampoline (LS 0xA70, intercepted in spu_channels.c) + the task
             * descriptor @0x2FB0. Adopted from JonathanDC64/ps3recomp (aaea4158).
             * Gate: default on for image 22 unless YDKJ_NO_CRI_CTX. */
            if (j->image_id == 22 && !getenv("YDKJ_NO_CRI_CTX")) {
                extern uint64_t spurs_pm_build_context(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t);
                extern uint32_t g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
                #define LSBE32(o,v) do{uint32_t _v=(v);ls[(o)+0]=(uint8_t)(_v>>24);ls[(o)+1]=(uint8_t)(_v>>16);ls[(o)+2]=(uint8_t)(_v>>8);ls[(o)+3]=(uint8_t)_v;}while(0)
                if (g_ydkj_real_taskset_ea && !getenv("YDKJ_MINIMAL_CTX")) {
                    /* REAL taskset context: build the SpursTasksetContext at LS 0x2700
                     * from the actual BE CellSpursTaskset (spurs ptr, args, TaskInfo)
                     * so the cri leaf reads valid data instead of my planted guesses. */
                    uint64_t elf = spurs_pm_build_context(ls, g_ydkj_real_taskset_ea, g_ydkj_real_taskid, 0, 0);
                    /* still plant the cri-specific task descriptor @0x2FB0 that build_context
                     * doesn't cover (cri func_00026DE0 reads it). */
                    LSBE32(0x2FB0, 0xFFFFFFFFu); LSBE32(0x2FB4, 0x400);
                    LSBE32(0x2FB8, 0x2700);      LSBE32(0x2FBC, 0x3000);
                    fprintf(stderr, "[cri] REAL SpursTasksetContext built from taskset 0x%08X task %u (elf=0x%llX)\n",
                            g_ydkj_real_taskset_ea, g_ydkj_real_taskid, (unsigned long long)elf);
                } else {
                    LSBE32(0x27C0, 0x100);     /* kernelMgmtAddr -> SPURS kernel ctx @LS 0x100 */
                    LSBE32(0x27C4, 0xA70);     /* syscallAddr -> HLE PM syscall trampoline (0xA70) */
                    LSBE32(0x2FB0, 0xFFFFFFFFu); LSBE32(0x2FB4, 0x400);
                    LSBE32(0x2FB8, 0x2700);      LSBE32(0x2FBC, 0x3000);
                    fprintf(stderr, "[cri] HLE cri-task ctx (minimal plant): syscallAddr@0x27C4=0xA70\n");
                }
                #undef LSBE32
                fflush(stderr);
            }
            /* Dump the 64-byte decode context the cri task will DMA from eaContext
             * (r3.word1 = args_ea). This is the job the game's cri_mpv PPU layer is
             * supposed to populate (video-data EA, output buffer). If it's zero/garbage
             * the PPU layer never wrote a real job -> the task decodes nothing. */
            if (j->image_id == 22 && j->args_ea) {
                extern uint8_t* vm_base;
                const uint8_t* c = vm_base + (j->args_ea & 0x0FFFFFFFu);
                fprintf(stderr, "[cri] eaContext=0x%08X 64B decode-context:", j->args_ea);
                for (int i = 0; i < 64; i++) { if ((i&15)==0) fprintf(stderr,"\n     +%02X:", i); fprintf(stderr, " %02X", c[i]); }
                fprintf(stderr, "\n"); fflush(stderr);
            }
            /* Async dispatch is the SPURS-task path: the entry expects the SPURS
             * task kernel ABI in r3 ({0x40 marker, eaContext, queue EA, ...}),
             * captured at dispatch time (j->r3) so it doesn't race the PPU
             * overwriting the stack-allocated context. */
            /* Generic SPURS taskset task (LBP's audio SPEEX/MultiStream tasks,
             * image 6/7 — anything but the cri image 22 handled above): the real
             * kernel runs the task UNDER the taskset policy, which plants a
             * SpursTasksetContext at LS 0x2700 (taskset header, TaskInfo, and
             * syscallAddr=0xA70) and enters it with r3 = task args, r4 = {spurs,
             * taskset args}. We dispatch the task ELF directly, so without this it
             * runs with r3=r4=0 and no context and SPINS FOREVER (observed: image
             * 6/7 ENTER + RUN, never RETURN, 0 DMA, PPU blocks on EventFlag 0x0100).
             * Build the context here from the real taskset CreateTask recorded;
             * spu_run_lifted_job_abi then sets r3/r4 off the 0xA70 sentinel.
             * Env-gated while bringing this up — default leaves every path as-is. */
            /* Timing probe: a real SPURS task is dispatched when work is signaled.
             * We dispatch immediately at CreateTask. If LBP fills the task's work
             * buffer slightly LATER, delaying the run lets it see real data (=>
             * timing bug, fix = defer/re-dispatch on signal); if it still loops on
             * a null base, the buffer is never filled (=> an audio HLE gap). */
            if (j->image_id != 22) {
                const char* d = getenv("LBP_TASK_DELAY");
                if (d && *d) {
#ifdef _WIN32
                    Sleep((unsigned)(atoi(d) * 1000));
#endif
                }
            }
            if (j->image_id != 22 && getenv("LBP_TASKSET")) {
                extern uint64_t spurs_pm_build_context(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t);
                /* Use the taskset+taskid captured for THIS job at dispatch (not the
                 * globals, which the next CreateTask clobbers -- the race that made
                 * both audio tasks run task 1's descriptor). */
                if (j->taskset_ea) {
                    spurs_pm_build_context(ls, j->taskset_ea, j->taskid, 0, 0);
                    fprintf(stderr, "[taskset] built SpursTasksetContext image=%d "
                            "taskset=0x%08X task=%u\n", j->image_id,
                            j->taskset_ea, j->taskid); fflush(stderr);
                }
            }
            int32_t rc = spu_run_lifted_job_abi(j->fn, ls, j->args_ea, j->image_id,
                                                1, j->have_r3 ? j->r3 : 0);
            /* YDKJ_CRI_RESUME: a real SPURS task is PERSISTENT -- on yield (num=0)
             * the kernel re-enters it when work is signaled. Our HLE runs it once,
             * so it polls the (concurrently PPU-updated) eaContext, finds no work,
             * yields and exits before the PPU marks work-ready. Approximate the
             * resume by re-running the task (fresh context re-read from eaContext)
             * in a bounded loop until it actually DECODES (a >256B video GET sets
             * g_cri_video_dma) or we time out. This does NOT fake data: the task
             * only decodes if the PPU genuinely populated real work meanwhile. */
            if (j->image_id == 22 && getenv("YDKJ_CRI_RESUME") && !g_cri_video_dma) {
                for (int attempt = 0; attempt < 400 && !g_cri_video_dma; attempt++) {
#ifdef _WIN32
                    Sleep(3);
#endif
                    memset(ls, 0, SPU_LS_SIZE);
                    uint32_t e2 = 0;
                    if (!spu_elf_load_to_ls(j->image, j->image_size, ls, &e2)) break;
                    rc = spu_run_lifted_job_abi(j->fn, ls, j->args_ea, j->image_id,
                                                1, j->have_r3 ? j->r3 : 0);
                    if (g_cri_video_dma) {
                        fprintf(stderr, "[cri] RESUME: cri task DECODED real video (attempt %d)\n", attempt);
                        break;
                    }
                }
                if (!g_cri_video_dma)
                    fprintf(stderr, "[cri] RESUME: no work-ready after 400 polls (PPU never marked work)\n");
                fflush(stderr);
            }
            /* YDKJ_CRI_WAKE probe: on cri task (image 22) completion, wake any PPU
             * completion-waiter (the SPU->PPU cellSpursEventFlag completion isn't
             * propagated yet). Tests whether the game then advances to draw content. */
            if (j->image_id == 22 && getenv("YDKJ_CRI_WAKE")) {
                extern void ydkj_wake_all_event_flags(void);
                ydkj_wake_all_event_flags();
                fprintf(stderr, "[cri] YDKJ_CRI_WAKE: woke all event-flag waiters on cri completion\n");
            }
            fprintf(stderr, "[spu_workload] async image=%d RETURNED rc=%d "
                    "(job ran to completion, did not loop)\n", j->image_id, rc);
        }
        free(ls);
    }
    free(j);
}

#ifdef _WIN32
static DWORD WINAPI spu_async_thread(LPVOID p) { spu_async_run((spu_async_job*)p); return 0; }
#else
static void* spu_async_thread(void* p) { spu_async_run((spu_async_job*)p); return NULL; }
#endif

int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size,
                                uint32_t args_ea)
{
    if (!image || image_size == 0) return 0;

    uint64_t fp = spu_workload_fingerprint(image, image_size);
    spu_lifted_entry_fn fn = NULL;
    int image_id = 0;
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fp) { fn = s_registry[i].fn; image_id = s_registry[i].image_id; break; }
    if (!fn) {
        fprintf(stderr, "[spu_workload] async dispatch MISS fp=0x%016llX size=%u\n",
                (unsigned long long)fp, image_size);
        return 0;
    }

    spu_async_job* j = (spu_async_job*)malloc(sizeof(*j));
    if (!j) return 0;
    j->image = image; j->image_size = image_size; j->args_ea = args_ea;
    j->fn = fn; j->image_id = image_id;
    /* Capture the taskset+taskid NOW (PPU thread, right after cellSpursCreateTask
     * set the globals for THIS task). Reading them later in the async thread races
     * the next CreateTask overwriting the single-slot globals -- with two audio
     * tasks that made both run task 1's descriptor. */
    { extern uint32_t g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
      j->taskset_ea = g_ydkj_real_taskset_ea; j->taskid = g_ydkj_real_taskid; }
    /* Capture the SPURS task r3 NOW (PPU thread, synchronous) from the game's
     * descriptor at eaContext+0x10 = {0x40-marker handle, workload EAs}; the
     * async SPU thread reading it later would race the PPU stack. word1 is
     * overridden to args_ea (eaContext) in spu_run_lifted_job_abi. */
    j->have_r3 = 0;
    if (args_ea) {
        extern uint8_t* vm_base;
        const uint8_t* c = vm_base + args_ea + 0x10;
        for (int k = 0; k < 4; k++)
            j->r3[k] = ((uint32_t)c[k*4]<<24)|((uint32_t)c[k*4+1]<<16)|
                       ((uint32_t)c[k*4+2]<<8)|c[k*4+3];
        if ((j->r3[0] >> 16) == 0x40) j->have_r3 = 1;   /* valid marker */
    }

    fprintf(stderr,
        "[spu_workload] dispatch HIT (async) fp=0x%016llX args=0x%08X image=%d -> spawning thread\n",
        (unsigned long long)fp, args_ea, image_id);
    if (args_ea) { extern uint8_t* vm_base; const uint8_t* c = vm_base + args_ea;
        static int _d=0; if (_d++ < 1) {
            /* Dump a larger window of the task context buffer + scan for any word
             * that looks like the LS[0xBEC0] target (i.e. a small LS-range value),
             * to see if the kernel-restored LS data lives here (real game data). */
            /* The policy module restores the task context from a save buffer into
             * LS 0xB200; LS[0xBEC0] = ctxbuf + 0xCC0. Check the heap-EA candidates
             * in the descriptor for a save buffer whose +0xCC0/+0xCC8 holds a
             * small LS pointer (a valid P for LS[0xBEC8]). */
            uint32_t cand[5];
            cand[0]=((uint32_t)c[0x14]<<24)|((uint32_t)c[0x15]<<16)|((uint32_t)c[0x16]<<8)|c[0x17];
            cand[1]=((uint32_t)c[0x1C]<<24)|((uint32_t)c[0x1D]<<16)|((uint32_t)c[0x1E]<<8)|c[0x1F];
            cand[2]=((uint32_t)c[0x98]<<24)|((uint32_t)c[0x99]<<16)|((uint32_t)c[0x9A]<<8)|c[0x9B];
            cand[3]=((uint32_t)c[0xB8]<<24)|((uint32_t)c[0xB9]<<16)|((uint32_t)c[0xBA]<<8)|c[0xBB];
            cand[4]=((uint32_t)c[0x16C]<<24)|((uint32_t)c[0x16D]<<16)|((uint32_t)c[0x16E]<<8)|c[0x16F];
            for (int ci=0; ci<5; ci++) {
                uint32_t ea=cand[ci]; if (!ea || ea>=0x10000000) continue;
                const uint8_t* b = vm_base + ea + 0xCC0;
                fprintf(stderr, "[ctxbuf cand 0x%08X +0xCC0]:", ea);
                for (int k=0;k<0x20;k+=4) {
                    uint32_t w=((uint32_t)b[k]<<24)|((uint32_t)b[k+1]<<16)|((uint32_t)b[k+2]<<8)|b[k+3];
                    fprintf(stderr, " %08X%s", w, (w>0&&w<0x40000)?"<LS":"");
                }
                fprintf(stderr, "\n");
            } } }

#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 1u << 20, spu_async_thread, j, 0, NULL);
    if (!th) { free(j); return 0; }
    CloseHandle(th);   /* detached */
#else
    pthread_t th;
    if (pthread_create(&th, NULL, spu_async_thread, j) != 0) { free(j); return 0; }
    pthread_detach(th);
#endif
    return 1;
}

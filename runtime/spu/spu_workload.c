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

spu_lifted_entry_fn spu_workload_find_img(uint64_t fingerprint, int* image_id_out)
{
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fingerprint) {
            if (image_id_out) *image_id_out = s_registry[i].image_id;
            return s_registry[i].fn;
        }
    if (image_id_out) *image_id_out = 0;
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

/* ---- Persistent per-taskset local store (LBP_PERSIST_LS) ------------------
 * Real SPURS time-multiplexes a taskset's tasks onto SPUs, saving/restoring a
 * shared taskset context in LS. Our default model runs each task ONCE on a
 * fresh calloc'd LS, so state one task establishes (e.g. FMOD's SPU DSP handler
 * table at LS 0x39Dxx, built by the init task) is invisible to the next task of
 * the same taskset, which reads the uninitialised image sentinel and
 * branch-to-0's. Give each taskset a PERSISTENT LS reused across its task
 * dispatches (image loaded once, state retained) and SERIALIZE its tasks (one
 * at a time) so they observe each other's setup without racing the shared
 * store -- a cooperative approximation of the kernel's context save/restore.
 * With LBP_WS_DRAIN (a task parked in WAIT_SIGNAL resumes + releases the
 * taskset, letting the next task run), the FMOD init + DSP tasks share one LS
 * and the handler table is populated when the DSP dispatch reads it. Opt-in;
 * default keeps per-dispatch fresh LS. */
typedef struct {
    uint32_t taskset_ea;   /* 0 = free slot */
    uint8_t* ls;           /* persistent LS (NULL until first task loads it) */
    int      loaded;       /* image loaded into ls */
#ifdef _WIN32
    SRWLOCK  lock;         /* serializes this taskset's task dispatches */
#else
    pthread_mutex_t lock;
#endif
} spu_ts_ls_slot;

#define SPU_TS_LS_MAX 24
static spu_ts_ls_slot s_ts_ls[SPU_TS_LS_MAX];
#ifdef _WIN32
static SRWLOCK s_ts_ls_dir = SRWLOCK_INIT;
#else
static pthread_mutex_t s_ts_ls_dir = PTHREAD_MUTEX_INITIALIZER;
#endif

static int spu_persist_ls_enabled(void)
{ static int _p = -1; if (_p < 0) _p = getenv("LBP_PERSIST_LS") ? 1 : 0; return _p; }

/* Find-or-create the slot for a taskset (directory lock held briefly). */
static spu_ts_ls_slot* ts_ls_get(uint32_t taskset_ea)
{
    spu_ts_ls_slot* slot = NULL;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_ts_ls_dir);
#else
    pthread_mutex_lock(&s_ts_ls_dir);
#endif
    for (int i = 0; i < SPU_TS_LS_MAX && !slot; i++)
        if (s_ts_ls[i].taskset_ea == taskset_ea) slot = &s_ts_ls[i];
    for (int i = 0; i < SPU_TS_LS_MAX && !slot; i++)
        if (s_ts_ls[i].taskset_ea == 0) {
            s_ts_ls[i].taskset_ea = taskset_ea;
#ifdef _WIN32
            InitializeSRWLock(&s_ts_ls[i].lock);
#else
            pthread_mutex_init(&s_ts_ls[i].lock, NULL);
#endif
            slot = &s_ts_ls[i];
        }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_ts_ls_dir);
#else
    pthread_mutex_unlock(&s_ts_ls_dir);
#endif
    return slot;
}

static void spu_async_run(spu_async_job* j)
{
    /* Persistent per-taskset LS: acquire (+serialize) the taskset's LS, or fall
     * back to a fresh per-dispatch LS when disabled / no taskset. */
    spu_ts_ls_slot* ts_slot = NULL;
    int did_load = 0;           /* whether THIS dispatch (re)loaded the image */
    if (spu_persist_ls_enabled() && j->taskset_ea && j->image_id != 22)
        ts_slot = ts_ls_get(j->taskset_ea);
    uint8_t* ls;
    if (ts_slot) {
#ifdef _WIN32
        AcquireSRWLockExclusive(&ts_slot->lock);   /* serialize this taskset */
#else
        pthread_mutex_lock(&ts_slot->lock);
#endif
        if (!ts_slot->ls) ts_slot->ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
        ls = ts_slot->ls;
        { static int _n = 0; if (_n++ < 24)
            fprintf(stderr, "[persist-ls] taskset=0x%08X task=%u image=%d %s\n",
                    j->taskset_ea, j->taskid, j->image_id,
                    ts_slot->loaded ? "REUSE (state retained)" : "first load"); fflush(stderr); }
    } else {
        ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    }
    if (ls) {
        uint32_t entry = 0;
        /* Load the image only on a taskset's FIRST task (or every dispatch when
         * not persisting) -- reusing preserves the prior task's LS state. */
        int loaded = (ts_slot && ts_slot->loaded)
                   ? 1
                   : (did_load = 1, spu_elf_load_to_ls(j->image, j->image_size, ls, &entry));
        if (ts_slot && did_load && loaded) ts_slot->loaded = 1;
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
                /* Run the taskset policy entry instead of the cri task entry. */
                extern int32_t spu_run_lifted_job_abi(void(*)(spu_context*), uint8_t*, uint32_t, int, int, uint32_t*);
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
            /* A generic SPURS taskset task (image != cri 22) MUST get its
             * SpursTasksetContext planted at LS 0x2700 -- the real kernel builds
             * it from the taskset before entry, and the task reads its args and
             * every DMA base pointer out of it. Without it the task runs with a
             * null base and DMAs EVERYTHING from EA 0 (observed on LBP's FMOD
             * mixer: 105k GETs from ea=0x0, degenerate handler targets ->
             * branch-to-0, and the PPU pump blocks forever on EventFlag 0x0100).
             * Planting it makes the mixer read the real FMOD control block
             * (0x0094F5xx) and the EventFlag handshake completes. Default ON;
             * LBP_NO_TASKSET restores the old direct-dispatch for comparison. */
            if (j->image_id != 22 && !getenv("LBP_NO_TASKSET")) {
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
            /* ---- Bink-layer probe + real-policy A/B (image 3 = binkspu) ----
             * (1) dump binkspu's Bink work-descriptor at its arg EA so we can see
             *     the output-plane EAs and the plane+0x50 "needs-decode" gate that
             *     gates the frame-output PUT (all prior PUTs went to the 0x927E80
             *     control block, never a plane). This tells us whether the missing
             *     output EA is a Bink/PPU-layer field (this descriptor) or a SPURS
             *     taskset-context field (LS 0x2700, below).
             * (2) LBP_REAL_POLICY: run the REAL lifted taskset policy to build its
             *     SpursTasksetContext and diff it against the C reimpl build_context
             *     planted in `ls` -- shows exactly which fields differ. */
            if (j->image_id == 3) {
                extern uint8_t* vm_base;
                uint32_t arg = j->args_ea;
                /* SPU DMA maps EAs as vm_base + (uint32_t)ea with NO masking
                 * (spu_dma.h:186); vm_base spans the full 4GB so VRAM 0xC0000000+
                 * is directly addressable. (The earlier &0x0FFFFFFF mask read main
                 * code at 0x10B88 instead of the real VRAM descriptor.) */
                #define RDBE32(base, off) (((uint32_t)(base)[(off)]<<24)|((uint32_t)(base)[(off)+1]<<16)| \
                                           ((uint32_t)(base)[(off)+2]<<8)|(base)[(off)+3])
                if (vm_base && arg >= 0x10000) {
                    const uint8_t* d = vm_base + arg;         /* FULL EA, no mask */
                    fprintf(stderr, "[bink-desc] arg=0x%08X 256B work-descriptor:", arg);
                    uint32_t words[64];
                    for (int i = 0; i < 256; i += 16) {
                        fprintf(stderr, "\n   +%03X:", i);
                        for (int k = 0; k < 16; k += 4) {
                            uint32_t w = RDBE32(d, i+k); words[(i+k)/4] = w;
                            fprintf(stderr, " %08X", w);
                        }
                    }
                    fprintf(stderr, "\n");
                    /* Follow each plausible pointer one level: dump +0x00..+0x60 so
                     * the output-plane EA + the plane+0x50 "needs-decode" gate show. */
                    for (int wi = 0; wi < 64; wi++) {
                        uint32_t p = words[wi];
                        if (p < 0x10000 || (p >= 0x50000000u && p < 0xC0000000u)) continue;
                        const uint8_t* q = vm_base + p;
                        fprintf(stderr, "  [desc+%02X]=0x%08X -> +00:%08X +40:%08X +50:%08X(gate) +54:%08X\n",
                                wi*4, p, RDBE32(q,0), RDBE32(q,0x40), RDBE32(q,0x50), RDBE32(q,0x54));
                    }
                    fflush(stderr);
                }
                if (getenv("LBP_REAL_POLICY") && j->taskset_ea) {
                    extern int spurs_run_taskset_policy_probe(uint32_t,uint32_t,uint32_t,
                                                              uint64_t,uint32_t,uint8_t*,uint32_t);
                    /* spurs EA from the taskset header lo32 @ +0x64 (be64 @0x60). */
                    uint32_t ts = j->taskset_ea;
                    uint32_t spurs_ea = 0;
                    if (vm_base) { const uint8_t* t = vm_base + ts;
                        spurs_ea = RDBE32(t, 0x64);
                        /* Why did the policy exit without dispatching? Dump the task
                         * bitsets it reads (task N = bit 127-N; task 2 = bit 125). */
                        fprintf(stderr, "[taskset-bits] RUNNING=%08X%08X READY=%08X%08X ENABLED=%08X%08X SIGNALLED=%08X%08X WAITING=%08X%08X\n",
                                RDBE32(t,0x00),RDBE32(t,0x04), RDBE32(t,0x10),RDBE32(t,0x14),
                                RDBE32(t,0x30),RDBE32(t,0x34), RDBE32(t,0x40),RDBE32(t,0x44),
                                RDBE32(t,0x50),RDBE32(t,0x54)); }
                    uint8_t real2700[0x180];
                    int st = spurs_run_taskset_policy_probe(ts, j->taskid, spurs_ea,
                                                            (uint64_t)ts, 3, real2700, sizeof real2700);
                    fprintf(stderr, "[real-pm] status=0x%X -- LS 0x2700 diff (build_context vs real policy):\n", st);
                    for (uint32_t o = 0; o < sizeof real2700; o += 4) {
                        uint32_t bc = ((uint32_t)ls[0x2700+o]<<24)|((uint32_t)ls[0x2700+o+1]<<16)|
                                      ((uint32_t)ls[0x2700+o+2]<<8)|ls[0x2700+o+3];
                        uint32_t rp = ((uint32_t)real2700[o]<<24)|((uint32_t)real2700[o+1]<<16)|
                                      ((uint32_t)real2700[o+2]<<8)|real2700[o+3];
                        if (bc != rp)
                            fprintf(stderr, "   0x%04X: build=%08X real=%08X\n", 0x2700+o, bc, rp);
                    }
                    /* EMPIRICAL TEST (LBP_POLICY_CTX): overlay the REAL policy's
                     * SpursTasksetContext onto binkspu's LS 0x2700, replacing the C
                     * reimpl, then let binkspu run with it. If the movie plane fills
                     * (not green) the policy's fuller ctx is the fix (H1); if still
                     * green, the frame-output gate is Bink-layer (H2). Keeps the
                     * build_context 0x100 kernel ctx (moduleId "TK") intact. */
                    if (getenv("LBP_POLICY_CTX")) {
                        memcpy(ls + 0x2700, real2700, sizeof real2700);
                        fprintf(stderr, "[real-pm] OVERLAID policy ctx onto binkspu LS 0x2700 (LBP_POLICY_CTX)\n");
                    }
                    fflush(stderr);
                }
                #undef RDBE32
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
        /* Persistent LS is retained in its taskset slot for the next task; a
         * per-dispatch LS is freed. */
        if (!ts_slot) free(ls);
    }
    if (ts_slot) {
#ifdef _WIN32
        ReleaseSRWLockExclusive(&ts_slot->lock);
#else
        pthread_mutex_unlock(&ts_slot->lock);
#endif
    }
    free(j);
}

#ifdef _WIN32
static DWORD WINAPI spu_async_thread(LPVOID p) {
    /* Reserve handler headroom so a stack overflow in lifted SPU code (e.g. a
     * runaway brsl recursion) can actually reach the STACKOVERFLOW reporter
     * instead of killing the process silently with 0x80000001. */
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }
    spu_async_run((spu_async_job*)p); return 0;
}
#else
static void* spu_async_thread(void* p) { spu_async_run((spu_async_job*)p); return NULL; }
#endif

int spu_workload_dispatch_job(const uint8_t* image, uint32_t image_size,
                              uint32_t job_ea, uint32_t job_desc_size)
{
    if (!image || image_size == 0) return 0;

    uint64_t fp = spu_workload_fingerprint(image, image_size);
    spu_lifted_entry_fn fn = NULL;
    int image_id = 0;
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fp) { fn = s_registry[i].fn; image_id = s_registry[i].image_id; break; }
    if (!fn) {
        fprintf(stderr, "[spurs-job] dispatch MISS fp=0x%016llX size=%u job=0x%08X\n",
                (unsigned long long)fp, image_size, job_ea);
        return 0;
    }
    fprintf(stderr, "[spurs-job] dispatch HIT fp=0x%016llX image=%d job=0x%08X\n",
            (unsigned long long)fp, image_id, job_ea);
    fflush(stderr);
    int rc = spu_run_spurs_job(fn, image_id, job_ea, job_desc_size);
    fprintf(stderr, "[spurs-job] job 0x%08X RETURNED rc=%d\n", job_ea, rc);
    fflush(stderr);
    return 1;
}

/* ---- SPURS task signal channel (real WAIT_SIGNAL semantics) --------------
 *
 * Mirrors the kernel/RPCS3 contract (spursTasksetProcessSyscall):
 *   - _cellSpursSendSignal / cellSpursEventFlagSet mark the task's bit in the
 *     taskset's `signalled` bitset (BE, in the GUEST CellSpursTaskset) and
 *     wake it if sleeping.
 *   - The WAIT_SIGNAL taskset syscall consumes a pending signal, or sleeps
 *     the task until one arrives (POLL_SIGNAL-then-wait, no lost wakeups:
 *     the guest bit is set under the same lock the waiter checks it).
 * The bitset is guest state (SPU-visible); the host lock/condvar exist only
 * to block and wake the task's host thread. */
#include "../../libs/spurs/spurs_taskset.h"

#ifdef _WIN32
static SRWLOCK            s_sig_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_sig_cv;   /* zero-init == CONDITION_VARIABLE_INIT */
#else
static pthread_mutex_t s_sig_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_sig_cv   = PTHREAD_COND_INITIALIZER;
#endif

/* Deliver a signal to a task (callable from any PPU/host thread). */
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId)
{
    if (!taskset_ea || taskId >= 128) return;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
    spurs_bitset_set(taskset_ea + CSTS_SIGNALLED, taskId);
    WakeAllConditionVariable(&s_sig_cv);
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
    spurs_bitset_set(taskset_ea + CSTS_SIGNALLED, taskId);
    pthread_cond_broadcast(&s_sig_cv);
    pthread_mutex_unlock(&s_sig_lock);
#endif
    { static int _n = 0; if (_n++ < 24)
        fprintf(stderr, "[spu_workload] signal task %u (taskset 0x%08X)\n",
                taskId, taskset_ea); fflush(stderr); }
}

/* WAIT_SIGNAL from the task side (runs ON the task's host thread, called by
 * the 0xA70 taskset-syscall intercept). Consumes a pending signal or blocks
 * until one is delivered. Returns 0 (the syscall's success rc). */
int spu_taskset_wait_signal(uint32_t taskset_ea, uint32_t taskId)
{
    if (!taskset_ea || taskId >= 128) return 0;
    { static int _n = 0; if (_n++ < 60 || (_n % 500) == 0)
        fprintf(stderr, "[spu_workload] WAIT_SIGNAL#%d enter task=%u taskset=0x%08X\n",
                _n, taskId, taskset_ea); }
    /* LBP_SYNC_ACK: taskset sync-lane bookkeeping. On real SPURS the taskset
     * POLICY MODULE advances the per-SPU progress lanes (sync+0x40+16*row:
     * ticket u16 @+0, consumer lanes @+2..) as tasks are processed; our HLE
     * runs the task on a dedicated thread and re-runs the PM ~never, so the
     * lanes froze and the skip path's min-of-lanes barrier (sub_4D3730) hung
     * forever. A task ENTERING WAIT_SIGNAL has by definition drained all work
     * dispatched to it, so advancing the active lanes to the published ticket
     * here is semantically the PM's bookkeeping, batched. Validation gate. */
    if (getenv("LBP_SYNC_ACK")) {
        extern uint32_t g_barrier_sync_watch;
        uint32_t b = g_barrier_sync_watch;
        if (b) {
            for (int row = 0; row < 4; row++) {
                uint32_t p = b + 0x40 + 16u * (uint32_t)row;
                extern uint8_t* vm_base;
                uint16_t ticket = (uint16_t)((vm_base[p] << 8) | vm_base[p+1]);
                for (int lane = 0; lane < 7; lane++) {
                    uint32_t la = p + 2 + 2u * (uint32_t)lane;
                    uint16_t cur = (uint16_t)((vm_base[la] << 8) | vm_base[la+1]);
                    if (cur != 0xFFFF && cur < ticket) {
                        vm_base[la]   = (uint8_t)(ticket >> 8);
                        vm_base[la+1] = (uint8_t)ticket;
                    }
                }
            }
            { static int _a = 0; if (_a++ < 8)
                fprintf(stderr, "[sync-ack] lanes advanced to tickets (sync=0x%08X)\n", b); }
        }
    }
    unsigned secs = 0;
    /* LBP_WS_DRAIN=<secs>: EXPERIMENT (general work-drained approximation).
     * A task ENTERING WAIT_SIGNAL has drained the work dispatched to it. In our
     * one-shot SPU model the producer task ran to completion already, so the
     * signal never arrives and the consumer blocks forever (movie/audio/loading
     * deadlock). If set, after <secs> of no real signal we RETURN rc=0 as if the
     * kernel resumed the task with "no pending work" -- lets it run its post-wait
     * path (complete its loop / set completion flags) instead of hanging. 0/unset
     * keeps the block-forever behaviour. Reversible, opt-in. */
    unsigned drain = 0; { const char* e = getenv("LBP_WS_DRAIN"); if (e) { drain = (unsigned)atoi(e); if (!drain) drain = 1; } }
    int drained = 0;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
    while (!spurs_bitset_test(taskset_ea + CSTS_SIGNALLED, taskId)) {
        if (!SleepConditionVariableSRW(&s_sig_cv, &s_sig_lock, 1000, 0)) {
            static int _n = 0;
            if (++secs && _n < 16) { _n++;
                fprintf(stderr, "[spu_workload] task %u (taskset 0x%08X) sleeping "
                        "%us in WAIT_SIGNAL\n", taskId, taskset_ea, secs); fflush(stderr); }
            if (drain && secs >= drain) { drained = 1; break; }
        }
    }
    if (!drained) spurs_bitset_clear(taskset_ea + CSTS_SIGNALLED, taskId);
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
    while (!spurs_bitset_test(taskset_ea + CSTS_SIGNALLED, taskId)) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
        if (pthread_cond_timedwait(&s_sig_cv, &s_sig_lock, &ts) != 0) {
            static int _n = 0;
            if (++secs && _n < 16) { _n++;
                fprintf(stderr, "[spu_workload] task %u (taskset 0x%08X) sleeping "
                        "%us in WAIT_SIGNAL\n", taskId, taskset_ea, secs); fflush(stderr); }
            if (drain && secs >= drain) { drained = 1; break; }
        }
    }
    if (!drained) spurs_bitset_clear(taskset_ea + CSTS_SIGNALLED, taskId);
    pthread_mutex_unlock(&s_sig_lock);
#endif
    if (drained) { static int _d = 0; if (_d++ < 24)
        fprintf(stderr, "[spu_workload] task %u (taskset 0x%08X) WS_DRAIN resume "
                "after %us (no signal)\n", taskId, taskset_ea, secs); fflush(stderr); }
    return 0;
}

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
    /* 256MB RESERVE (not commit): the Bink SPU decoder's per-macroblock brsl
     * chains are deep host call chains with fat lifted frames -- a 1MB stack
     * blew the guard page (STATUS_GUARD_PAGE_VIOLATION 0x80000001, silent
     * process death) the moment the real decode path first executed. */
    HANDLE th = CreateThread(NULL, 256u << 20, spu_async_thread, j,
                             STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!th) { free(j); return 0; }
    CloseHandle(th);   /* detached */
#else
    pthread_t th;
    if (pthread_create(&th, NULL, spu_async_thread, j) != 0) { free(j); return 0; }
    pthread_detach(th);
#endif
    return 1;
}

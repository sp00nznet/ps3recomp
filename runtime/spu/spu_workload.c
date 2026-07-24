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
                /* Load the TASKSET POLICY module (libsre 0x30023680 -> LS 0xA00) alongside
                 * the cri task (already at 0x3000), then INTERPRET the policy entry (0xA00):
                 * the policy builds the 0x2700 task-API jump table + dispatches the ready cri
                 * task itself, so the task's computed branches (which branch-to-0 when we run
                 * the task directly) resolve through the policy's live table. Runs the policy
                 * INTERPRETED (not lifted) because the policy has the same computed-branch /
                 * jump-table code the static lift can't resolve. */
                extern uint8_t* vm_base;
                extern uint64_t spurs_pm_build_context(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t);
                extern uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
                extern uint32_t vm_read32(uint32_t); extern void vm_write32(uint32_t,uint32_t);
                extern uint64_t vm_read64(uint32_t);
                if (!g_ydkj_real_taskset_ea) { const char* _cts=getenv("YDKJ_CRI_TS");
                    if (_cts && *_cts) { g_ydkj_real_taskset_ea=(uint32_t)strtoul(_cts,0,16); g_ydkj_real_taskid=0; } }
                if (g_ydkj_real_taskset_ea) {
                    /* wait for the LLE task-add to populate TaskInfo[].elf, then promote
                     * PENDING_READY -> READY so the policy finds a runnable task. */
                    uint32_t ti_elf = g_ydkj_real_taskset_ea + 0x80 + g_ydkj_real_taskid*48 + 0x10;
                    int i=0; for (; i<6000 && (vm_read64(ti_elf)&~7ull)==0; i++) {
#ifdef _WIN32
                        Sleep(1);
#else
                        { struct timespec _t={0,1000000}; nanosleep(&_t,0); }
#endif
                    }
                    for (int w=0; w<4; w++){ uint32_t pend=vm_read32(g_ydkj_real_taskset_ea+0x20+w*4);
                        if (pend) vm_write32(g_ydkj_real_taskset_ea+0x10+w*4,
                                    vm_read32(g_ydkj_real_taskset_ea+0x10+w*4)|pend); }
                    fprintf(stderr,"[cri] CRI_TASKSET: taskset elf=0x%llX ready=0x%08X after %dms\n",
                        (unsigned long long)(vm_read64(ti_elf)&~7ull), vm_read32(g_ydkj_real_taskset_ea+0x10), i);
                }
                if (vm_base) memcpy(ls + 0xA00, vm_base + 0x30023680, 0x2200);
                if (g_ydkj_real_taskset_ea)
                    spurs_pm_build_context(ls, g_ydkj_real_taskset_ea, g_ydkj_real_taskid, 0, 0);
                uint32_t inst = g_ydkj_real_spurs_ea ? g_ydkj_real_spurs_ea : 0x40009F00u;
                uint8_t* p = ls + 0x1C0;
                p[0]=0; p[1]=0; p[2]=0; p[3]=0;                 /* hi32 of u64 */
                p[4]=(uint8_t)(inst>>24); p[5]=(uint8_t)(inst>>16);
                p[6]=(uint8_t)(inst>>8);  p[7]=(uint8_t)inst;   /* lo32 */
                /* INTERPRET the policy entry (kernel->policy handoff: r80=0x100 context base,
                 * r3=SpursTasksetContext @0x2700). image_id=-1 => pure interpretation. */
                spu_context pctx; spu_context_init(&pctx, 0); pctx.image_id = -1;
                pctx.gpr[1]._u32[0] = SPU_LS_SIZE - 0x10;
                memcpy(pctx.ls, ls, SPU_LS_SIZE);
                pctx.gpr[80]._u32[0] = 0x100;
                pctx.gpr[3]._u32[0]  = 0x2700;
                fprintf(stderr, "[cri] YDKJ_CRI_TASKSET: policy@0xA00 resident, interpreting policy entry 0xA00 (inst=0x%08X)\n", inst);
                fflush(stderr);
                uint32_t pstop = spu_interp_run(&pctx, 0xA00);
                memcpy(ls, pctx.ls, SPU_LS_SIZE);
                fprintf(stderr, "[cri] CRI_TASKSET: policy interp halted LSA=0x%X stop=0x%X (video_dma=%d)\n",
                        pstop, pctx.stop_code, g_cri_video_dma);
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
                /* YDKJ_CRI_TS: the cri DECODE taskset is created by LLE (not the HLE CreateTask),
                 * so g_ydkj_real_taskset_ea stays 0 and we fall to the minimal/garbage plant. Let
                 * the caller supply the cri taskset EA (observed 0x40131000) so build_context reads
                 * the REAL TaskInfo (eaElf/eaContext/args) the LLE create wrote. */
                if (!g_ydkj_real_taskset_ea) { const char* _cts=getenv("YDKJ_CRI_TS");
                    if (_cts && *_cts) { g_ydkj_real_taskset_ea=(uint32_t)strtoul(_cts,0,16); g_ydkj_real_taskid=0;
                        fprintf(stderr,"[cri] YDKJ_CRI_TS: forcing cri taskset=0x%08X\n", g_ydkj_real_taskset_ea); } }
                /* YDKJ_CRI_WAIT: the LLE task-add (0x1D46FEDF) writes the task's ELF into
                 * the taskset TaskInfo AFTER this SPU workload is dispatched (~150 log lines
                 * earlier than the create). Reading the taskset once here gets elf=0. We run
                 * on a detached thread, so poll (up to ~6s) for the PPU to populate
                 * TaskInfo[taskid].elf before building the context. Opt out: YDKJ_NO_CRI_WAIT. */
                if (g_ydkj_real_taskset_ea && !getenv("YDKJ_NO_CRI_WAIT")) {
                    extern uint64_t vm_read64(uint32_t);
                    uint32_t ti_elf = g_ydkj_real_taskset_ea + 0x80 + g_ydkj_real_taskid*48 + 0x10;
                    int waited=0; for (; waited<6000; waited++) { if ((vm_read64(ti_elf)&~7ull)!=0) break;
#ifdef _WIN32
                        Sleep(1);
#else
                        { struct timespec _t={0,1000000}; nanosleep(&_t,0); }
#endif
                    }
                    fprintf(stderr,"[cri] CRI_WAIT: taskset TaskInfo[%u].elf=0x%llX after %d ms\n",
                            g_ydkj_real_taskid,(unsigned long long)(vm_read64(ti_elf)&~7ull),waited);
                }
                /* YDKJ_CRI_READY: the LLE task-add leaves tasks in PENDING_READY+ENABLED
                 * (bitsets @0x20/0x30) but NOT READY (@0x10); the real SPURS kernel promotes
                 * pending->ready on its scheduling pass. Our policy interp reads the taskset
                 * once, so promote pending->ready here (per 32-bit word) so the policy finds a
                 * runnable task instead of exiting at entry. Opt out: YDKJ_NO_CRI_READY. */
                if (g_ydkj_real_taskset_ea && !getenv("YDKJ_NO_CRI_READY")) {
                    extern uint32_t vm_read32(uint32_t); extern void vm_write32(uint32_t,uint32_t);
                    /* Complete the FULL pending->running transition the kernel does, matching
                     * the working RPCS3 SPU0 dump: RUNNING(0x00) set, READY(0x10) set,
                     * PENDING(0x20) CLEARED. Our old code left the task PENDING and not
                     * RUNNING, so the task's poll (func_00026E80 reads RUNNING) saw "not
                     * running" and busy-spun forever. */
                    for (int w=0; w<4; w++){ uint32_t pend=vm_read32(g_ydkj_real_taskset_ea+0x20+w*4);
                        if (pend) {
                            vm_write32(g_ydkj_real_taskset_ea+0x10+w*4,   /* READY  |= pending */
                                       vm_read32(g_ydkj_real_taskset_ea+0x10+w*4)|pend);
                            vm_write32(g_ydkj_real_taskset_ea+0x00+w*4,   /* RUNNING|= pending */
                                       vm_read32(g_ydkj_real_taskset_ea+0x00+w*4)|pend);
                            vm_write32(g_ydkj_real_taskset_ea+0x20+w*4, 0); /* PENDING = 0 */
                        } }
                    fprintf(stderr,"[cri] CRI_READY: pending->running (run=0x%08X ready=0x%08X pend=0x%08X)\n",
                        vm_read32(g_ydkj_real_taskset_ea+0x00), vm_read32(g_ydkj_real_taskset_ea+0x10),
                        vm_read32(g_ydkj_real_taskset_ea+0x20));
                }
                if (g_ydkj_real_taskset_ea && !getenv("YDKJ_MINIMAL_CTX")) {
                    /* REAL taskset context: build the SpursTasksetContext at LS 0x2700
                     * from the actual BE CellSpursTaskset (spurs ptr, args, TaskInfo)
                     * so the cri leaf reads valid data instead of my planted guesses. */
                    uint64_t elf = spurs_pm_build_context(ls, g_ydkj_real_taskset_ea, g_ydkj_real_taskid, 0, 0);
                    /* --- Golden-reference context fields for the CRI task, recovered from a
                     * WORKING RPCS3 SPU-LS snapshot via caner's rpcs3-guest-memory-dumper fork
                     * (github.com/canersaka/rpcs3-guest-memory-dumper): YDKJ BLUS30569, SPU0
                     * "CellSpursKernel0" mid-cri-decode. These are what the cri task's context
                     * validator (func_00026E80/F18/FC4) reads; our values differed and tripped
                     * its 0x80410911 error path. CRI-SCOPED (image 22 only) -- overriding these
                     * in the shared build_context breaks the audio SPURS task. */
                    LSBE32(0x2840, 0x53505552u); LSBE32(0x2844, 0x53544153u); /* moduleId "SPURSTASK MODULE" */
                    LSBE32(0x2848, 0x4B204D4Fu); LSBE32(0x284C, 0x44554C45u);
                    LSBE32(0x27A0, 0); LSBE32(0x27A4, 0);       /* TI_LS_PATTERN = 0 (build_context forced all-ones; validator andc r21) */
                    LSBE32(0x27A8, 0); LSBE32(0x27AC, 0);
                    LSBE32(0x27D0, 0x1F);                       /* dmaTagId = 0x1F (build_context wrote 0) */
                    /* still plant the cri-specific task descriptor @0x2FB0 that build_context
                     * doesn't cover (cri func_00026DE0 reads it). */
                    LSBE32(0x2FB0, 0); LSBE32(0x2FB4, 0);   /* RPCS3 dump: descriptor word0/word1 = 0 (was 0xFFFFFFFF/0x400) */
                    LSBE32(0x2FB8, 0x2700);      LSBE32(0x2FBC, 0x3000);
                    fprintf(stderr, "[cri] REAL SpursTasksetContext built from taskset 0x%08X task %u (elf=0x%llX)\n",
                            g_ydkj_real_taskset_ea, g_ydkj_real_taskid, (unsigned long long)elf);
                    if (getenv("YDKJ_CRI_DUMP")) { extern uint8_t* vm_base;
                        #define RD32(ea) (((uint32_t)vm_base[(ea)]<<24)|((uint32_t)vm_base[(ea)+1]<<16)|((uint32_t)vm_base[(ea)+2]<<8)|vm_base[(ea)+3])
                        /* Dump the actual structures (committed, safe) instead of a RAM scan --
                         * the earlier scan was vacuous (ppu_vm_size==0 at runtime). Show the taskset
                         * header+TaskInfo (find eaElf/eaContext) and the SPURS instance workload
                         * words (w0/w4 -- observed EMPTY, so the kernel has nothing to dispatch). */
                        uint32_t ts = g_ydkj_real_taskset_ea;
                        fprintf(stderr,"[cri-dump] taskset 0x%08X first 0x80 bytes:\n", ts);
                        for(uint32_t o=0;o<0x80;o+=16){ fprintf(stderr,"  +0x%02X:",o);
                            for(uint32_t k=0;k<16;k+=4) fprintf(stderr," %08X", RD32(ts+o+k)); fprintf(stderr,"\n"); }
                        /* scan the taskset's own 0x1000 bytes for the cri ELF ptr 0x4F5F80 (this
                         * region IS committed -- we just read it above) */
                        uint32_t hits=0;
                        for(uint32_t a=ts; a<ts+0x2000; a+=4){ uint32_t w=RD32(a);
                            if((w&~0xFu)==0x004F5F80u){ fprintf(stderr,"  eaElf 0x4F5F80 REF @0x%08X = 0x%08X\n",a,w); hits++; } }
                        fprintf(stderr,"[cri-dump] eaElf refs in taskset: %u\n", hits);
                        /* SPURS instance workload state */
                        for(uint32_t inst=0x40009D00u; inst<=0x40009F00u; inst+=0x200){
                            fprintf(stderr,"[cri-dump] SPURS instance @0x%08X: w0=%08X w4=%08X w8=%08X wC=%08X w10=%08X w14=%08X\n",
                                inst, RD32(inst), RD32(inst+4), RD32(inst+8), RD32(inst+0xC), RD32(inst+0x10), RD32(inst+0x14)); }
                        fflush(stderr);
                        #undef RD32
                    }
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
             * SpursTasksetContext planted at LS 0x2700 -- the real kernel builds it
             * from the taskset before entry, and the task reads its args and every
             * DMA base pointer out of it. Without it the task DMAs from EA 0 and the
             * PPU pump blocks forever on the EventFlag. Default ON (sagemono 35c2767);
             * LBP_NO_TASKSET restores the old opt-in dispatch for comparison. */
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
            /* YDKJ_CRI_INTERP: run the cri task (image 22) through the SPU
             * INTERPRETER on the prepared LS (SpursTasksetContext already planted
             * at 0x2700 by the taskset path above). The LIFTED image branch-to-0's
             * on the SPURS task-API jump table @0x2700 / computed `bi $reg` that the
             * static lift can't resolve; the interpreter executes those from live
             * LS. Same SPURS task ABI r3 as the lifted dispatch (word0=handle,
             * word1=eaContext, word2/3=queue/lock). Merged from the SPU-interp
             * branch (aa3a85a). Best paired with YDKJ_CRI_TS=0x4000C900. */
            if (j->image_id == 22 && getenv("YDKJ_CRI_INTERP")) {
                spu_context ictx;
                spu_context_init(&ictx, 0);
                /* image_id = -1 => PURE interpretation. image 22 HAS lifted functions, and
                 * spu_interp_run rejoins the compiled fast path the instant it finds one at
                 * the PC (spu_interp.c:340, gated on image_id>=0) -- so with image_id=22 it
                 * returned at the entry without interpreting anything. Pure interp is the whole
                 * point here (execute the jump-table/computed-branch code the static lift can't
                 * resolve), so mark it un-lifted. */
                ictx.image_id = -1;
                ictx.gpr[1]._u32[0] = SPU_LS_SIZE - 0x10;   /* stack top */
                memcpy(ictx.ls, ls, SPU_LS_SIZE);
                /* SPURS task entry ABI: r3 = the task's CellSpursTaskArgument (16 bytes)
                 * from the taskset TaskInfo (TI_ARGS @ +0x00), NOT the stale j->r3 that was
                 * captured at dispatch time before the LLE task-add populated the task. Read
                 * it from the real taskset now that CRI_WAIT confirmed it's written. */
                extern uint32_t g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
                if (g_ydkj_real_taskset_ea) {
                    extern uint32_t vm_read32(uint32_t);
                    uint32_t ti = g_ydkj_real_taskset_ea + 0x80 + g_ydkj_real_taskid*48;
                    ictx.gpr[3]._u32[0] = vm_read32(ti+0x0);
                    ictx.gpr[3]._u32[1] = vm_read32(ti+0x4);
                    ictx.gpr[3]._u32[2] = vm_read32(ti+0x8);
                    ictx.gpr[3]._u32[3] = vm_read32(ti+0xC);
                } else if (j->have_r3) {
                    ictx.gpr[3]._u32[0] = j->r3[0];     /* 0x40-marker handle   */
                    ictx.gpr[3]._u32[1] = j->args_ea;   /* eaContext (DMA'd 1st)*/
                    ictx.gpr[3]._u32[2] = j->r3[2];     /* queue/lock EA        */
                    ictx.gpr[3]._u32[3] = j->r3[3];
                }
                fprintf(stderr, "[cri] YDKJ_CRI_INTERP: interpret image 22 entry=0x%X "
                        "r3=[%08X %08X %08X %08X]\n", entry, ictx.gpr[3]._u32[0],
                        ictx.gpr[3]._u32[1], ictx.gpr[3]._u32[2], ictx.gpr[3]._u32[3]);
                /* Is there real code at the entry, and is any task actually READY/PENDING in
                 * the taskset? (halt-at-entry stop 0 = policy found nothing to run.) */
                fprintf(stderr, "[cri] LS[entry 0x%X]: %02X%02X%02X%02X %02X%02X%02X%02X   taskset bitsets ready=%08X pend=%08X enabled=%08X run=%08X\n",
                        entry, ictx.ls[entry],ictx.ls[entry+1],ictx.ls[entry+2],ictx.ls[entry+3],
                        ictx.ls[entry+4],ictx.ls[entry+5],ictx.ls[entry+6],ictx.ls[entry+7],
                        g_ydkj_real_taskset_ea?vm_read32(g_ydkj_real_taskset_ea+0x10):0,
                        g_ydkj_real_taskset_ea?vm_read32(g_ydkj_real_taskset_ea+0x20):0,
                        g_ydkj_real_taskset_ea?vm_read32(g_ydkj_real_taskset_ea+0x30):0,
                        g_ydkj_real_taskset_ea?vm_read32(g_ydkj_real_taskset_ea+0x00):0);
                fflush(stderr);
                uint32_t stop_lsa = spu_interp_run(&ictx, entry);
                extern uint32_t g_spu_interp_last_pc; extern uint64_t g_spu_interp_steps;
                memcpy(ls, ictx.ls, SPU_LS_SIZE);
                /* NB: spu_interp_run returns the STOP_CODE, not the PC. Report the real halt
                 * PC (g_spu_interp_last_pc) + step count so "stopped" isn't misread as "at 0". */
                fprintf(stderr, "[cri] YDKJ_CRI_INTERP: image 22 interp halted at pc=0x%X "
                        "stop_code=0x%X after %llu steps (video_dma=%d) [ret=0x%X]\n",
                        g_spu_interp_last_pc, ictx.stop_code, (unsigned long long)g_spu_interp_steps,
                        g_cri_video_dma, stop_lsa);
                fflush(stderr);
                free(ls); free(j); return;
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
    unsigned secs = 0;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
    while (!spurs_bitset_test(taskset_ea + CSTS_SIGNALLED, taskId)) {
        if (!SleepConditionVariableSRW(&s_sig_cv, &s_sig_lock, 1000, 0)) {
            static int _n = 0;
            if (++secs && _n < 16) { _n++;
                fprintf(stderr, "[spu_workload] task %u (taskset 0x%08X) sleeping "
                        "%us in WAIT_SIGNAL\n", taskId, taskset_ea, secs); fflush(stderr); }
        }
    }
    spurs_bitset_clear(taskset_ea + CSTS_SIGNALLED, taskId);
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
        }
    }
    spurs_bitset_clear(taskset_ea + CSTS_SIGNALLED, taskId);
    pthread_mutex_unlock(&s_sig_lock);
#endif
    return 0;
}


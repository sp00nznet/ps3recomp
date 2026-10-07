/*
 * prx_loader.c — see prx_loader.h for the design.
 *
 * Two steps per module: place the relocated image into guest RAM at its load
 * base, then register every lifted function in the host dispatch table.
 */
#include "prx_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime's guest memory: host_ptr = vm_base + guest_addr. Declared by
 * runtime/memory/vm.h; we only need the symbol, so extern it directly to keep
 * the loader independent of the memory backend's init path. */
extern uint8_t* vm_base;

static void prx_watch_add(const prx_module* m);      /* PS3_PRX_WATCH, below */
static void prx_watch_remove(const prx_module* m);
static void prx_watch_start(void);

/* ---- export registry ---------------------------------------------------- *
 * Flat NID -> guest-address table, filled as modules load. A handful of system
 * PRXs export a few hundred symbols total, so a linear array with linear lookup
 * is plenty (lookups happen once per import resolution, not per call). */
typedef struct { uint32_t nid; uint32_t guest_addr; } prx_export_reg_entry;
#define PRX_EXPORT_REG_CAP 2048
static prx_export_reg_entry s_export_reg[PRX_EXPORT_REG_CAP];
static uint32_t             s_export_reg_n = 0;

static void prx_export_register(uint32_t nid, uint32_t guest_addr)
{
    /* Last writer wins if two modules export the same NID (matches the PS3
     * loader's link order). Overwrite an existing entry rather than duplicate. */
    for (uint32_t i = 0; i < s_export_reg_n; i++) {
        if (s_export_reg[i].nid == nid) {
            s_export_reg[i].guest_addr = guest_addr;
            return;
        }
    }
    if (s_export_reg_n < PRX_EXPORT_REG_CAP)
        s_export_reg[s_export_reg_n++] = (prx_export_reg_entry){ nid, guest_addr };
    else
        fprintf(stderr, "[prx] export registry full, dropping NID 0x%08X\n", nid);
}

uint32_t prx_resolve_export(uint32_t nid)
{
    for (uint32_t i = 0; i < s_export_reg_n; i++)
        if (s_export_reg[i].nid == nid)
            return s_export_reg[i].guest_addr;
    return 0;
}

uint32_t prx_export_registry_count(void) { return s_export_reg_n; }

static void prx_export_unregister(uint32_t nid, uint32_t guest_addr)
{
    for (uint32_t i = 0; i < s_export_reg_n; i++) {
        if (s_export_reg[i].nid == nid && s_export_reg[i].guest_addr == guest_addr) {
            s_export_reg[i] = s_export_reg[--s_export_reg_n];
            return;
        }
    }
}

int prx_place_module(const prx_module* m, prx_register_fn reg)
{
    if (!m || !m->image || !m->image_size || !vm_base) return 0;
    memcpy(vm_base + m->base, m->image, m->image_size);
    if (reg)
        for (uint64_t i = 0; i < m->func_count; i++)
            if (m->funcs[i].func) reg((uint32_t)m->funcs[i].addr, m->funcs[i].func);
    return 1;
}

void prx_publish_exports(const prx_module* m)
{
    for (uint32_t i = 0; m && m->exports && i < m->export_count; i++)
        if (m->exports[i].nid) prx_export_register(m->exports[i].nid, m->base + m->exports[i].vaddr);
    if (m) { prx_watch_add(m); prx_watch_start(); }
}

void prx_withdraw_exports(const prx_module* m)
{
    for (uint32_t i = 0; m && m->exports && i < m->export_count; i++)
        if (m->exports[i].nid) prx_export_unregister(m->exports[i].nid, m->base + m->exports[i].vaddr);
    if (m) prx_watch_remove(m);
}

/* ---- PS3_PRX_WATCH: catch whatever overwrites a loaded module's OPDs ------ *
 * A published export is an OPD inside the placed image, and nothing rewrites
 * an OPD after relocation. inFamous (T-0017) intermittently finds liblv2's
 * lwmutex/lwcond OPDs reading zero mid-boot, after which every thread that
 * takes a lock calls address 0. Polling every export against the original
 * image bytes says WHEN that happens and what the overwrite looks like (its
 * extent and contents), which is what names the writer: a DMA, a memset, a
 * stray guest store. Off unless PS3_PRX_WATCH is set; POSIX only for now. */
#define PRX_WATCH_CAP 32
static const prx_module* volatile s_watch[PRX_WATCH_CAP];

static void prx_watch_add(const prx_module* m)
{
    for (int i = 0; i < PRX_WATCH_CAP; i++) if (s_watch[i] == m) return;
    for (int i = 0; i < PRX_WATCH_CAP; i++)
        if (!s_watch[i]) { s_watch[i] = m; return; }
}
static void prx_watch_remove(const prx_module* m)
{
    for (int i = 0; i < PRX_WATCH_CAP; i++) if (s_watch[i] == m) s_watch[i] = NULL;
}

#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static double prx_watch_now_s(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* One overwritten run: grow from `off` while guest bytes differ from the
 * image, tolerating gaps of up to 32 equal bytes (a store pattern that
 * happens to match the original for a few bytes is still one write). */
static void prx_watch_report(const prx_module* m, uint32_t off, uint32_t nid, double t0)
{
    const uint8_t* g = vm_base + m->base;
    uint32_t lo = off, hi = off + 8, gap;
    for (gap = 0; lo > 0 && gap <= 32; ) { lo--; if (g[lo] != m->image[lo]) gap = 0; else gap++; }
    lo += gap;
    for (gap = 0; hi < m->image_size && gap <= 32; hi++) { if (g[hi] != m->image[hi]) gap = 0; else gap++; }
    hi -= gap;
    fprintf(stderr, "[prx-watch] t=+%.3fs %s OPD of NID 0x%08X at 0x%08X changed;"
                    " overwritten run 0x%08X..0x%08X (%u bytes)\n",
            prx_watch_now_s() - t0, m->name ? m->name : "?", nid, m->base + off,
            m->base + lo, m->base + hi, hi - lo);
    for (uint32_t a = lo & ~15u, rows = 0; a < hi && rows < 16; a += 16, rows++) {
        char line[160]; int p = snprintf(line, sizeof line, "[prx-watch]   %08X now:", m->base + a);
        for (uint32_t k = 0; k < 16; k += 4)
            p += snprintf(line + p, sizeof line - p, " %02X%02X%02X%02X",
                          g[a+k], g[a+k+1], g[a+k+2], g[a+k+3]);
        p += snprintf(line + p, sizeof line - p, "  was:");
        for (uint32_t k = 0; k < 16; k += 4)
            p += snprintf(line + p, sizeof line - p, " %02X%02X%02X%02X",
                          m->image[a+k], m->image[a+k+1], m->image[a+k+2], m->image[a+k+3]);
        fprintf(stderr, "%s\n", line);
    }
    fflush(stderr);
}

static void* prx_watch_thread(void* arg)
{
    (void)arg;
    const double t0 = prx_watch_now_s();
    int reports = 0;
    fprintf(stderr, "[prx-watch] polling published OPDs every 1 ms\n");
    while (reports < 16) {
        for (int i = 0; i < PRX_WATCH_CAP; i++) {
            const prx_module* m = s_watch[i];
            if (!m || !m->exports) continue;
            for (uint32_t j = 0; j < m->export_count; j++) {
                uint32_t off = m->exports[j].vaddr;
                if (!m->exports[j].nid || off + 8 > m->image_size) continue;
                if (memcmp(vm_base + m->base + off, m->image + off, 8) == 0) continue;
                prx_watch_report(m, off, m->exports[j].nid, t0);
                /* Report a module once: the first hit's run covers its
                 * neighbours, and a flood would bury the timing. */
                s_watch[i] = NULL;
                reports++;
                break;
            }
        }
        usleep(1000);
    }
    return NULL;
}

static void prx_watch_start(void)
{
    static int started = 0;
    if (started || !getenv("PS3_PRX_WATCH")) return;
    started = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, prx_watch_thread, NULL) == 0) pthread_detach(t);
}
#else
static void prx_watch_start(void) {}
#endif

/* ---- statically lifted modules, by file name ----------------------------- */
#define PRX_STATIC_CAP 32
static const prx_static_module* s_static[PRX_STATIC_CAP];
static uint32_t                 s_static_n = 0;

void prx_static_register(const prx_static_module* m)
{
    if (m && s_static_n < PRX_STATIC_CAP) s_static[s_static_n++] = m;
}

const prx_static_module* prx_static_find(const char* file)
{
    for (uint32_t i = 0; file && i < s_static_n; i++)
        if (strcmp(s_static[i]->file, file) == 0) return s_static[i];
    return NULL;
}

prx_load_result prx_load_module(const prx_module* m, prx_register_fn reg)
{
    prx_load_result r;
    memset(&r, 0, sizeof(r));

    if (!m || !m->image || m->image_size == 0) {
        fprintf(stderr, "[prx] load failed: null/empty module\n");
        return r;
    }
    if (!vm_base) {
        fprintf(stderr, "[prx] load failed: vm_base not initialized "
                        "(call vm_init before loading PRX modules)\n");
        return r;
    }

    r.base       = m->base;
    r.image_size = m->image_size;

    /* Step A — place the relocated image into guest RAM at the load base.
     * The bytes already carry base-applied addresses (prx_relocate.py), so the
     * module's data, OPDs and pointer tables are immediately consistent with
     * the lifted code and the OPD-walking dispatcher. */
    memcpy(vm_base + m->base, m->image, m->image_size);

    /* Step B — register every lifted function at its guest address. The lift
     * baked the load base in (--base B), so e->addr is already B + offset and
     * needs no adjustment here. A null host function (rare: a table hole) is
     * skipped rather than registered. */
    if (reg) {
        for (uint64_t i = 0; i < m->func_count; i++) {
            const prx_func_entry* e = &m->funcs[i];
            if (!e->func)
                continue;
            reg((uint32_t)e->addr, e->func);
            r.funcs_registered++;
        }
    }

    /* Step C — publish exports as NID -> (base + vaddr) so a title's import
     * resolver can dispatch into this module by NID. Function exports point at
     * an OPD in the freshly-placed image; ps3_indirect_call walks it. */
    uint32_t exports_published = 0;
    if (m->exports) {
        for (uint32_t i = 0; i < m->export_count; i++) {
            const prx_export* e = &m->exports[i];
            if (e->nid == 0)
                continue;
            prx_export_register(e->nid, m->base + e->vaddr);
            exports_published++;
        }
    }

    prx_watch_add(m);
    prx_watch_start();

    r.ok = 1;
    fprintf(stderr,
            "[prx] loaded %-12s base=0x%08X size=%u (0x%X) funcs=%u/%llu exports=%u\n",
            m->name ? m->name : "?", m->base, m->image_size, m->image_size,
            r.funcs_registered, (unsigned long long)m->func_count,
            exports_published);
    return r;
}

uint8_t* prx_image_load_file(const char* path, uint32_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[prx] cannot open image '%s'\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "[prx] short read on '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (out_size)
        *out_size = (uint32_t)n;
    return buf;
}

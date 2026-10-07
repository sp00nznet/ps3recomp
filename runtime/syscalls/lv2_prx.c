/*
 * ps3recomp - lv2's PRX loader: the _sys_prx_* syscalls liblv2 uses to load,
 * start, stop and unload modules, and _sys_process_get_paramsfo, which its
 * process start-up also calls. RPCS3's lv2 (rpcs3/Emu/Cell/lv2/sys_prx.cpp)
 * is the reference for the kernel's behaviour.
 *
 * Loading a module does not read a file. A module lifted at build time has
 * registered itself by file name (runtime/prx, tools/gen_prx_module.py), and
 * loading it places its relocated image and binds its lifted functions. A
 * firmware module with no lifted build is answered by the HLE libraries, the
 * same way RPCS3 handles the modules it does not run LLE: the load succeeds
 * with an id and nothing behind it, and starting it has no entry to run.
 *
 * Exports are linked when a module is started (cmd 1 of _sys_prx_start_module)
 * and unlinked when it is stopped, as in lv2.
 *
 * Differences from lv2 that remain:
 *   - A lifted module has one fixed load address, so it can be loaded once at
 *     a time; a second load while the first is loaded fails with
 *     CELL_PRX_ERROR_ERROR (lv2 would load a second copy elsewhere).
 *   - Fixed-address loads (flags & 1) are refused with ENOSYS, as RPCS3 does
 *     for a process without a fixed-allocation segment.
 */
#include "lv2_syscall_table.h"
#include "../prx/prx_loader.h"
#include "../../include/ps3emu/error_codes.h"
#include "../platform/win32_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t* vm_base;
extern void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

#define PRX_ERROR_ERROR            0x80011001u
#define PRX_ERROR_UNKNOWN_MODULE   0x8001112Eu
#define PRX_ERROR_NOT_STARTED      0x80011134u
#define PRX_ERROR_ALREADY_STOPPED  0x80011135u
#define PRX_ERROR_CAN_NOT_STOP     0x80011136u
#define PRX_ERROR_NOT_REMOVABLE    0x80011138u
#define PRX_ERROR_ALREADY_STOPPING 0x8001113Fu
#define PRX_ERROR_ELF_IS_REGISTERED 0x80011910u

enum { ST_INITIALIZED, ST_STARTING, ST_STARTED, ST_STOPPING, ST_STOPPED, ST_DESTROYED };

typedef struct {
    int                      in_use;
    uint32_t                 id;
    int                      state;
    const prx_static_module* m;          /* NULL: answered by HLE */
    char                     path[128];
} lv2_prx;

#define LV2_PRX_MAX 64
static lv2_prx  s_prx[LV2_PRX_MAX];
static uint32_t s_next_id = 1;
static SRWLOCK  s_lock = SRWLOCK_INIT;

static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t rd64(uint32_t ea) { return (uint64_t)rd32(ea) << 32 | rd32(ea + 4); }
static void     wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void     wr64(uint32_t ea, uint64_t v) { wr32(ea, (uint32_t)(v >> 32)); wr32(ea + 4, (uint32_t)v); }

static void reg_fn(uint32_t addr, void (*fn)(void*)) { ppu_register_function(addr, (void (*)(ppu_context*))fn); }

static lv2_prx* find(uint32_t id)
{
    for (int i = 0; i < LV2_PRX_MAX; i++)
        if (s_prx[i].in_use && s_prx[i].id == id) return &s_prx[i];
    return NULL;
}

static const char* basename_of(const char* path)
{
    const char* s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Load one module by path; returns its id (> 0) or a negative CELL error. */
static int64_t prx_load(uint32_t path_ea, uint64_t flags)
{
    if (flags) return (flags & ~1ull) ? (int32_t)CELL_EINVAL : (int32_t)CELL_ENOSYS;
    if (!path_ea) return (int32_t)CELL_EFAULT;
    char path[128];
    strncpy(path, (const char*)(vm_base + path_ea), sizeof path - 1);
    path[sizeof path - 1] = 0;

    const prx_static_module* m = prx_static_find(basename_of(path));
    const int firmware = strncmp(path, "/dev_flash/sys/external/", 24) == 0;
    if (!m && !firmware) return (int32_t)CELL_ENOENT;

    AcquireSRWLockExclusive(&s_lock);
    if (m)
        for (int i = 0; i < LV2_PRX_MAX; i++)
            if (s_prx[i].in_use && s_prx[i].m == m) {
                ReleaseSRWLockExclusive(&s_lock);
                fprintf(stderr, "[lv2_prx] %s is already loaded; a second copy is not supported\n", path);
                return (int32_t)PRX_ERROR_ERROR;
            }
    lv2_prx* p = NULL;
    for (int i = 0; i < LV2_PRX_MAX && !p; i++)
        if (!s_prx[i].in_use) p = &s_prx[i];
    if (!p) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_ENOMEM; }
    memset(p, 0, sizeof *p);
    p->in_use = 1;
    p->id     = 0x23000000u | (__atomic_fetch_add(&s_next_id, 1, __ATOMIC_RELAXED) << 8);
    p->state  = ST_INITIALIZED;
    p->m      = m;
    snprintf(p->path, sizeof p->path, "%s", path);
    if (m) prx_place_module(&m->mod, reg_fn);
    const uint32_t id = p->id;
    ReleaseSRWLockExclusive(&s_lock);
    fprintf(stderr, "[lv2_prx] %s module \"%s\" (id=0x%X)\n", m ? "loaded" : "HLE", path, id);
    return id;
}

static int64_t prx_unload(uint32_t id)
{
    AcquireSRWLockExclusive(&s_lock);
    lv2_prx* p = find(id);
    if (!p) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)PRX_ERROR_UNKNOWN_MODULE; }
    if (p->state != ST_INITIALIZED && p->state != ST_STOPPED) {
        ReleaseSRWLockExclusive(&s_lock);
        return (int32_t)PRX_ERROR_NOT_REMOVABLE;
    }
    p->in_use = 0;
    ReleaseSRWLockExclusive(&s_lock);
    return CELL_OK;
}

/* _sys_prx_load_module(path, flags, pOpt) -- 480; on_memcontainer -- 497 */
static int64_t sys_prx_load_module(ppu_context* ctx)
{
    return prx_load((uint32_t)ctx->gpr[3], ctx->gpr[4]);
}

static int64_t sys_prx_load_module_on_memcontainer(ppu_context* ctx)
{
    return prx_load((uint32_t)ctx->gpr[3], ctx->gpr[5]);
}

/* Load `count` modules from an array of 64-bit path pointers. On a failure
 * the ones already loaded are unloaded and id_list is filled with -1. */
static int64_t prx_load_list(int32_t count, uint32_t list, uint64_t flags, uint32_t ids)
{
    if (flags) return (flags & ~1ull) ? (int32_t)CELL_EINVAL : (int32_t)CELL_ENOSYS;
    for (int32_t i = 0; i < count; i++) {
        const int64_t r = prx_load((uint32_t)rd64(list + 8u * (uint32_t)i), 0);
        if (r < 0) {
            while (--i >= 0) prx_unload(rd32(ids + 4u * (uint32_t)i));
            memset(vm_base + ids, 0xFF, 4u * (uint32_t)count);
            return r;
        }
        wr32(ids + 4u * (uint32_t)i, (uint32_t)r);
    }
    return CELL_OK;
}

/* _sys_prx_load_module_list(count, path_list, flags, pOpt, id_list) -- 465 */
static int64_t sys_prx_load_module_list(ppu_context* ctx)
{
    return prx_load_list((int32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], ctx->gpr[5], (uint32_t)ctx->gpr[7]);
}

/* ..._on_memcontainer(count, path_list, mem_ct, flags, pOpt, id_list) -- 466 */
static int64_t sys_prx_load_module_list_on_memcontainer(ppu_context* ctx)
{
    return prx_load_list((int32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], ctx->gpr[6], (uint32_t)ctx->gpr[8]);
}

/* _sys_prx_start_module(id, flags, pOpt) -- 481
 * pOpt { u64 size; u64 cmd; u64 entry; u64 res; u64 entry2; }
 *   cmd 1: INITIALIZED -> STARTING, link the exports, return the start entry
 *          (~0 if none); liblv2 then calls it.
 *   cmd 2: res is what the start entry returned: 0 (resident) -> STARTED;
 *          anything with a nonzero low word -> unload, and return it. */
static int64_t sys_prx_start_module(ppu_context* ctx)
{
    const uint32_t id = (uint32_t)ctx->gpr[3], opt = (uint32_t)ctx->gpr[5];
    if (!id || !opt) return (int32_t)CELL_EINVAL;
    AcquireSRWLockExclusive(&s_lock);
    lv2_prx* p = find(id);
    if (!p) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_ESRCH; }
    switch (rd64(opt + 8) & 0xF) {
    case 1:
        if (p->state != ST_INITIALIZED) {
            const int st = p->state;
            ReleaseSRWLockExclusive(&s_lock);
            return st == ST_DESTROYED ? (int32_t)CELL_ESRCH : (int32_t)PRX_ERROR_ERROR;
        }
        p->state = ST_STARTING;
        if (p->m) prx_publish_exports(&p->m->mod);
        wr64(opt + 0x10, p->m && p->m->start_opd ? p->m->start_opd : ~0ull);
        if (rd64(opt) != 0x20) wr64(opt + 0x20, ~0ull);       /* no prologue */
        ReleaseSRWLockExclusive(&s_lock);
        return CELL_OK;
    case 2: {
        const uint64_t res = rd64(opt + 0x18);
        if (res == 0) {
            p->state = ST_STARTED;
            ReleaseSRWLockExclusive(&s_lock);
            return CELL_OK;
        }
        if (res & 0xFFFFFFFFu) {
            p->state = ST_STOPPED;
            if (p->m) prx_withdraw_exports(&p->m->mod);
            ReleaseSRWLockExclusive(&s_lock);
            prx_unload(id);
            return (int32_t)res;
        }
        ReleaseSRWLockExclusive(&s_lock);
        return CELL_OK;
    }
    default:
        ReleaseSRWLockExclusive(&s_lock);
        return (int32_t)PRX_ERROR_ERROR;
    }
}

/* _sys_prx_stop_module(id, flags, pOpt) -- 482
 *   cmd 1: STARTED -> STOPPING, return the stop entry (~0 if none);
 *   cmd 2: res 0 -> unlink the exports, STOPPED; res 1 -> CAN_NOT_STOP;
 *   cmd 4: return the stop entry; cmd 8: accepted, no effect. */
static int64_t sys_prx_stop_module(ppu_context* ctx)
{
    const uint32_t id = (uint32_t)ctx->gpr[3], opt = (uint32_t)ctx->gpr[5];
    AcquireSRWLockExclusive(&s_lock);
    lv2_prx* p = find(id);
    int64_t r = CELL_OK;
    if (!p) r = (int32_t)CELL_ESRCH;
    else if (!opt) r = (int32_t)CELL_EINVAL;
    else {
        const uint64_t cmd = rd64(opt + 8) & 0xF;
        if (cmd == 1 || cmd == 4 || cmd == 8) {
            switch (p->state) {
            case ST_INITIALIZED: r = (int32_t)PRX_ERROR_NOT_STARTED; break;
            case ST_STOPPED:     r = (int32_t)PRX_ERROR_ALREADY_STOPPED; break;
            case ST_STOPPING:    r = (int32_t)PRX_ERROR_ALREADY_STOPPING; break;
            case ST_STARTING:    r = (int32_t)PRX_ERROR_ERROR; break;
            case ST_DESTROYED:   r = (int32_t)CELL_ESRCH; break;
            default:
                if (cmd == 1) p->state = ST_STOPPING;
                if (cmd != 8) {
                    wr64(opt + 0x10, p->m && p->m->stop_opd ? p->m->stop_opd : ~0ull);
                    if (rd64(opt) != 0x20) wr64(opt + 0x20, ~0ull);   /* no epilogue */
                }
            }
        } else if (cmd == 2) {
            const uint64_t res = rd64(opt + 0x18);
            if (res == 0) {
                if (p->m) prx_withdraw_exports(&p->m->mod);
                p->state = ST_STOPPED;
            } else if (res == 1) {
                r = (int32_t)PRX_ERROR_CAN_NOT_STOP;
            }
        } else {
            r = (int32_t)PRX_ERROR_ERROR;
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
    return r;
}

/* _sys_prx_unload_module(id, flags, pOpt) -- 483 */
static int64_t sys_prx_unload_module(ppu_context* ctx)
{
    return prx_unload((uint32_t)ctx->gpr[3]);
}

/* _sys_prx_register_module(name, opt) -- 484. liblv2 registers the process's
 * own ELF ("cellProcessElf"); only a VSH process may have the kernel link it
 * by hand (type & 1), anyone else is told it is already registered. */
static int64_t sys_prx_register_module(ppu_context* ctx)
{
    const uint32_t opt = (uint32_t)ctx->gpr[4];
    if (!opt) return (int32_t)CELL_EINVAL;
    const uint64_t size = rd64(opt);
    uint64_t type;
    if (size == 0x1C || size == 0x20) type = 0;
    else if (size == 0x30) type = rd64(opt + 8);
    else return (int32_t)CELL_EINVAL;
    return (type & 1) ? (int32_t)PRX_ERROR_ELF_IS_REGISTERED : CELL_OK;
}

/* query_module 485, register_library 486, unregister_library 487,
 * link_library 488, unlink_library 489, query_library 490: lv2 links every
 * import by NID through ps3_hle_call, so these have nothing to do. */
static int64_t sys_prx_ok(ppu_context* ctx) { (void)ctx; return CELL_OK; }

/* _sys_prx_get_module_list(flags, pInfo) -- 494
 * pInfo { u64 size (0x20); u32 pad; u32 max; u32 count; u32 idlist; u32 unk; }
 * Only with flags & 2; liblv2 itself is not listed (as in RPCS3). */
static int64_t sys_prx_get_module_list(ppu_context* ctx)
{
    const uint64_t flags = ctx->gpr[3];
    const uint32_t info = (uint32_t)ctx->gpr[4];
    if (!(flags & 2)) return CELL_OK;
    if (rd64(info) != 0x20) return CELL_OK;
    const uint32_t max = rd32(info + 0xC), list = rd32(info + 0x14);
    uint32_t n = 0;
    AcquireSRWLockShared(&s_lock);
    for (int i = 0; i < LV2_PRX_MAX && n < max; i++)
        if (s_prx[i].in_use && strcmp(basename_of(s_prx[i].path), "liblv2.sprx") != 0)
            wr32(list + 4u * n++, s_prx[i].id);
    ReleaseSRWLockShared(&s_lock);
    wr32(info + 0x10, n);
    return CELL_OK;
}

/* _sys_prx_get_module_info(id, flags, pOpt) -- 495
 * pOpt { u64 size (0x10); u32 info; }
 * info { u64 size (0x48, or 0x58 with the v2 tail); char name[30];
 *        char version[2]; u32 attr; u32 start; u32 stop; u32 nsegs;
 *        u32 filename; u32 filename_size; u32 segments; u32 segments_num;
 *        [v2: u32 exports, exports_size, imports, imports_size] } */
static int64_t sys_prx_get_module_info(ppu_context* ctx)
{
    const uint32_t id = (uint32_t)ctx->gpr[3], opt = (uint32_t)ctx->gpr[5];
    if (!opt) return (int32_t)CELL_EFAULT;
    if (rd64(opt) != 0x10) return (int32_t)CELL_EINVAL;
    const uint32_t info = rd32(opt + 8);
    if (!info) return (int32_t)CELL_EFAULT;
    const uint64_t isz = rd64(info);
    if (isz != 0x48 && isz != 0x58) return (int32_t)CELL_EINVAL;
    AcquireSRWLockShared(&s_lock);
    lv2_prx* p = find(id);
    if (!p) { ReleaseSRWLockShared(&s_lock); return (int32_t)PRX_ERROR_UNKNOWN_MODULE; }
    char* name = (char*)(vm_base + info + 8);
    memset(name, 0, 30);
    if (p->m && p->m->module_name) strncpy(name, p->m->module_name, 29);
    wr32(info + 0x2C, p->m ? p->m->start_opd : 0);
    wr32(info + 0x30, p->m ? p->m->stop_opd : 0);
    wr32(info + 0x34, p->m ? 1 : 0);
    const uint32_t fn = rd32(info + 0x38), fnsz = rd32(info + 0x3C);
    if (fn && fnsz) {
        strncpy((char*)(vm_base + fn), p->path, fnsz);
        vm_base[fn + fnsz - 1] = 0;
    }
    const uint32_t segs = rd32(info + 0x40);
    if (segs) {
        uint32_t n = 0;
        if (p->m && rd32(info + 0x44)) {
            wr64(segs + 0x00, p->m->mod.base);
            wr64(segs + 0x08, p->m->mod.image_size);
            wr64(segs + 0x10, p->m->mod.image_size);
            wr64(segs + 0x18, 0);
            wr64(segs + 0x20, 1);
            n = 1;
        }
        wr32(info + 0x44, n);
    }
    ReleaseSRWLockShared(&s_lock);
    return CELL_OK;
}

/* _sys_prx_get_module_id_by_name(name, flags, pOpt) -- 496: by module_info name */
static int64_t sys_prx_get_module_id_by_name(ppu_context* ctx)
{
    const uint32_t name = (uint32_t)ctx->gpr[3];
    if (!name) return (int32_t)CELL_EINVAL;
    int64_t r = (int32_t)PRX_ERROR_UNKNOWN_MODULE;
    AcquireSRWLockShared(&s_lock);
    for (int i = 0; i < LV2_PRX_MAX; i++)
        if (s_prx[i].in_use && s_prx[i].m && s_prx[i].m->module_name &&
            strncmp((const char*)(vm_base + name), s_prx[i].m->module_name, 28) == 0) {
            r = s_prx[i].id;
            break;
        }
    ReleaseSRWLockShared(&s_lock);
    return r;
}

/* _sys_prx_get_module_id_by_address(addr) -- 461 */
static int64_t sys_prx_get_module_id_by_address(ppu_context* ctx)
{
    const uint32_t a = (uint32_t)ctx->gpr[3];
    int64_t r = (int32_t)PRX_ERROR_UNKNOWN_MODULE;
    AcquireSRWLockShared(&s_lock);
    for (int i = 0; i < LV2_PRX_MAX; i++)
        if (s_prx[i].in_use && s_prx[i].m &&
            a >= s_prx[i].m->mod.base && a < s_prx[i].m->mod.base + s_prx[i].m->mod.image_size) {
            r = s_prx[i].id;
            break;
        }
    ReleaseSRWLockShared(&s_lock);
    return r;
}

/* _sys_process_get_paramsfo(buffer) -- 30: 0x40 bytes, the title id at +1.
 * A process with no title id (a bare ELF) gets ENOENT. PS3_TITLE_ID supplies
 * one. */
static int64_t sys_process_get_paramsfo(ppu_context* ctx)
{
    const char* tid = getenv("PS3_TITLE_ID");
    if (!tid || !*tid) return (int32_t)CELL_ENOENT;
    const uint32_t buf = (uint32_t)ctx->gpr[3];
    memset(vm_base + buf, 0, 0x40);
    memcpy(vm_base + buf + 1, tid, strlen(tid) < 9 ? strlen(tid) : 9);
    return CELL_OK;
}

/* Process start with liblv2 lifted: the kernel loads liblv2 with the process
 * and links its exports before anything runs (RPCS3's ppu_load_exec). Returns
 * liblv2's module_start OPD, which becomes the main thread's entry, or 0 when
 * liblv2 is not lifted into this build. */
uint32_t lv2_prx_boot_liblv2(void)
{
    const prx_static_module* m = prx_static_find("liblv2.sprx");
    if (!m || !m->start_opd) return 0;
    AcquireSRWLockExclusive(&s_lock);
    lv2_prx* p = &s_prx[0];
    memset(p, 0, sizeof *p);
    p->in_use = 1;
    p->id     = 0x23000000u | (__atomic_fetch_add(&s_next_id, 1, __ATOMIC_RELAXED) << 8);
    p->state  = ST_STARTED;
    p->m      = m;
    snprintf(p->path, sizeof p->path, "/dev_flash/sys/external/liblv2.sprx");
    ReleaseSRWLockExclusive(&s_lock);
    prx_place_module(&m->mod, reg_fn);
    prx_publish_exports(&m->mod);
    fprintf(stderr, "[lv2_prx] liblv2 loaded with the process (id=0x%X), entry OPD 0x%08X\n",
            p->id, m->start_opd);
    return m->start_opd;
}

void lv2_prx_register_syscalls(lv2_syscall_table* tbl)
{
    lv2_syscall_register(tbl, 30,  sys_process_get_paramsfo);
    lv2_syscall_register(tbl, 461, sys_prx_get_module_id_by_address);
    lv2_syscall_register(tbl, 465, sys_prx_load_module_list);
    lv2_syscall_register(tbl, 466, sys_prx_load_module_list_on_memcontainer);
    lv2_syscall_register(tbl, 480, sys_prx_load_module);
    lv2_syscall_register(tbl, 481, sys_prx_start_module);
    lv2_syscall_register(tbl, 482, sys_prx_stop_module);
    lv2_syscall_register(tbl, 483, sys_prx_unload_module);
    lv2_syscall_register(tbl, 484, sys_prx_register_module);
    lv2_syscall_register(tbl, 485, sys_prx_ok);
    lv2_syscall_register(tbl, 486, sys_prx_ok);
    lv2_syscall_register(tbl, 487, sys_prx_ok);
    lv2_syscall_register(tbl, 488, sys_prx_ok);
    lv2_syscall_register(tbl, 489, sys_prx_ok);
    lv2_syscall_register(tbl, 490, sys_prx_ok);
    lv2_syscall_register(tbl, 494, sys_prx_get_module_list);
    lv2_syscall_register(tbl, 495, sys_prx_get_module_info);
    lv2_syscall_register(tbl, 496, sys_prx_get_module_id_by_name);
    lv2_syscall_register(tbl, 497, sys_prx_load_module_on_memcontainer);
}

/*
 * ps3recomp - sys_vm virtual memory syscalls (implementation)
 *
 * sys_vm gives a process a demand-paged virtual region backed by a smaller
 * physical commitment (vsize virtual / psize physical, paged by the OS).
 * Our guest space is one flat host reservation, so committing the whole
 * vsize satisfies the contract exactly: every page is always resident and
 * the paging-control syscalls (touch/lock/flush/...) are trivially done.
 *
 * Call contract verified against the Yakuza EBOOT itself (func_00072510):
 *   r3 = vsize   (the game rounds it up to a 32 MB multiple first)
 *   r4 = psize   (the game rounds it up to a 64 KB multiple first)
 *   r5 = cid     (memory container, 0xFFFFFFFF = none)
 *   r6 = flag    (page size: SYS_MEMORY_PAGE_SIZE_64K = 0x200)
 *   r7 = policy  (1 = auto recommended)
 *   r8 = guest u32* that receives the mapped base address
 * The game reads the u32 back, passes it to sys_vm_touch (306) and uses it
 * as a heap arena base. RPCS3's sys_vm.cpp used as a read-only cross-check.
 *
 * sys_vm_test (311) and sys_vm_get_statistics (312) are intentionally NOT
 * registered: both write result structs whose layout we have not verified
 * from a primary source, and the default table handler logs any call loudly
 * instead of corrupting memory with a guessed layout.
 */

#include "sys_vm.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_vm_map_info g_sys_vm_maps[SYS_VM_MAX];

/* Bump pointer inside the sys_vm window (mappings are never reused) */
static uint32_t s_vm_bump_ptr = SYS_VM_REGION_BASE;

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

static sys_vm_map_info* find_map(uint32_t addr)
{
    for (int i = 0; i < SYS_VM_MAX; i++) {
        if (g_sys_vm_maps[i].active && g_sys_vm_maps[i].addr == addr)
            return &g_sys_vm_maps[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * sys_vm_memory_map (300) / sys_vm_memory_map_different (313)
 *
 * r3 = vsize, r4 = psize, r5 = cid, r6 = flag, r7 = policy
 * r8 = pointer to receive mapped base address (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_vm_memory_map(ppu_context* ctx)
{
    uint32_t vsize    = LV2_ARG_U32(ctx, 0);
    uint32_t psize    = LV2_ARG_U32(ctx, 1);
    uint32_t addr_out = LV2_ARG_PTR(ctx, 5);

    if (vsize == 0 || psize == 0 ||
        (vsize % SYS_VM_VSIZE_ALIGN) != 0 ||
        (psize % SYS_VM_PSIZE_ALIGN) != 0 ||
        vsize > (SYS_VM_REGION_END - SYS_VM_REGION_BASE))
        return (int64_t)(int32_t)CELL_EINVAL;

    if (s_vm_bump_ptr + vsize > SYS_VM_REGION_END)
        return (int64_t)(int32_t)CELL_ENOMEM;

    int slot = -1;
    for (int i = 0; i < SYS_VM_MAX; i++) {
        if (!g_sys_vm_maps[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_ENOMEM;

    uint32_t base = s_vm_bump_ptr;
    if (vm_commit(base, vsize) != CELL_OK)
        return (int64_t)(int32_t)CELL_ENOMEM;
    /* freshly committed pages are already zeroed by the OS */
    s_vm_bump_ptr += vsize;

    sys_vm_map_info* m = &g_sys_vm_maps[slot];
    m->active = 1;
    m->addr   = base;
    m->vsize  = vsize;
    m->psize  = psize;

    if (addr_out != 0)
        write_be32(addr_out, base);

    fprintf(stderr, "[sys_vm] memory_map(vsize=0x%X, psize=0x%X) -> 0x%08X\n",
            vsize, psize, base);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_vm_unmap (301)
 *
 * r3 = mapped base address
 * -----------------------------------------------------------------------*/
int64_t sys_vm_unmap(ppu_context* ctx)
{
    uint32_t addr = LV2_ARG_U32(ctx, 0);

    sys_vm_map_info* m = find_map(addr);
    if (!m)
        return (int64_t)(int32_t)CELL_EINVAL;

    /* Keep the pages committed: stale guest pointers into the region then
     * read zeros/old data instead of faulting the whole process. The window
     * is bump-allocated, so the range is never handed out again. */
    m->active = 0;

    fprintf(stderr, "[sys_vm] unmap(0x%08X)\n", addr);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_vm_append_memory (302) / sys_vm_return_memory (303)
 *
 * r3 = mapped base address, r4 = size
 * Adjusts the physical backing of a mapping. All our pages are always
 * committed, so this is pure bookkeeping.
 * -----------------------------------------------------------------------*/
int64_t sys_vm_append_memory(ppu_context* ctx)
{
    uint32_t addr = LV2_ARG_U32(ctx, 0);
    uint32_t size = LV2_ARG_U32(ctx, 1);

    sys_vm_map_info* m = find_map(addr);
    if (!m || size == 0 || (size % SYS_VM_PSIZE_ALIGN) != 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    m->psize += size;
    return CELL_OK;
}

int64_t sys_vm_return_memory(ppu_context* ctx)
{
    uint32_t addr = LV2_ARG_U32(ctx, 0);
    uint32_t size = LV2_ARG_U32(ctx, 1);

    sys_vm_map_info* m = find_map(addr);
    if (!m || size == 0 || (size % SYS_VM_PSIZE_ALIGN) != 0 || size > m->psize)
        return (int64_t)(int32_t)CELL_EINVAL;

    m->psize -= size;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Paging-control hints (304-310): lock/unlock/touch/flush/invalidate/
 * store/sync. With every page permanently committed these are satisfied
 * by doing nothing.
 * -----------------------------------------------------------------------*/
int64_t sys_vm_advisory_nop(ppu_context* ctx)
{
    (void)ctx;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_vm_init(lv2_syscall_table* tbl)
{
    memset(g_sys_vm_maps, 0, sizeof(g_sys_vm_maps));
    s_vm_bump_ptr = SYS_VM_REGION_BASE;

    lv2_syscall_register(tbl, SYS_VM_MEMORY_MAP,           sys_vm_memory_map);
    lv2_syscall_register(tbl, SYS_VM_MEMORY_MAP_DIFFERENT, sys_vm_memory_map);
    lv2_syscall_register(tbl, SYS_VM_UNMAP,                sys_vm_unmap);
    lv2_syscall_register(tbl, SYS_VM_APPEND_MEMORY,        sys_vm_append_memory);
    lv2_syscall_register(tbl, SYS_VM_RETURN_MEMORY,        sys_vm_return_memory);
    lv2_syscall_register(tbl, SYS_VM_LOCK,                 sys_vm_advisory_nop);
    lv2_syscall_register(tbl, SYS_VM_UNLOCK,               sys_vm_advisory_nop);
    lv2_syscall_register(tbl, SYS_VM_TOUCH,                sys_vm_advisory_nop);
    lv2_syscall_register(tbl, SYS_VM_FLUSH,                sys_vm_advisory_nop);
    lv2_syscall_register(tbl, SYS_VM_INVALIDATE,           sys_vm_advisory_nop);
    lv2_syscall_register(tbl, SYS_VM_STORE,                sys_vm_advisory_nop);
    lv2_syscall_register(tbl, SYS_VM_SYNC,                 sys_vm_advisory_nop);
    /* 311 sys_vm_test / 312 sys_vm_get_statistics intentionally left on the
     * default handler (loud ENOSYS log) until their out-struct layouts are
     * verified from a primary source. */
}

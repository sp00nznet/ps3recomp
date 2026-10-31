/*
 * ps3recomp - Memory management syscalls (implementation)
 */

#include "sys_memory.h"
#include <stdio.h>
#include <string.h>
#include "../../include/ps3emu/log_first.h"   /* relative: tools/test_*memory*.py compile this file alone */

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_mem_alloc_info       g_sys_mem_allocs[SYS_MEMORY_ALLOC_MAX];
sys_mem_container_info   g_sys_mem_containers[SYS_MEMORY_CONTAINER_MAX];
sys_mmapper_shared_info  g_sys_mmapper_shared[SYS_MMAPPER_SHARED_MAX];

/* Bump allocator for main memory pool.
 * Starts after a reasonable offset to avoid the low addresses used by ELF. */
uint32_t g_sys_mem_bump_ptr = 0;

/* Total user memory size (default 213 MB, after kernel reservation) */
#define SYS_MEM_USER_TOTAL  (213 * 1024 * 1024)
#define SYS_MEMORY_CONTAINER_DEFAULT 0xFFFFFFFFu   /* SYS_MEMORY_CONTAINER_ID_INVALID: the default container */

/* LV2 user memory and mapper reservations occupy separate address domains.
 * User allocations start above the executable region. Mapper addresses may
 * carry high address bits; ordinary user allocations must not inherit them. */
#define SYS_MEM_ALLOC_BASE  0x10000000u
#define SYS_MEM_ALLOC_END   0x20000000u
#define SYS_MAPPER_BASE     0x40000000u
#define SYS_MAPPER_END      0xC0000000u
static uint32_t s_mapper_bump_ptr = SYS_MAPPER_BASE;
typedef struct mapper_reservation {
    uint32_t start, end, next;
} mapper_reservation;
static mapper_reservation s_mapper_reservations[SYS_MEMORY_ALLOC_MAX];


static uint32_t s_total_allocated = 0;
static uint64_t (*s_image_query)(uint64_t addr, uint64_t end);

void sys_memory_set_image_query(uint64_t (*query)(uint64_t addr, uint64_t end))
{
    s_image_query = query;
}

/* Guest threads are real host threads; serialize the bump allocator. */
#ifdef _WIN32
static SRWLOCK s_bump_lock = SRWLOCK_INIT;
static void bump_lock(void)   { AcquireSRWLockExclusive(&s_bump_lock); }
static void bump_unlock(void) { ReleaseSRWLockExclusive(&s_bump_lock); }
#else
#include <pthread.h>
static pthread_mutex_t s_bump_mtx = PTHREAD_MUTEX_INITIALIZER;
static void bump_lock(void)   { pthread_mutex_lock(&s_bump_mtx); }
static void bump_unlock(void) { pthread_mutex_unlock(&s_bump_mtx); }
#endif

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * sys_memory_allocate
 *
 * r3 = size
 * r4 = flags (page size)
 * r5 = pointer to receive allocated address (u32*)
 * -----------------------------------------------------------------------*/
/* Find an aligned gap among live user allocations. Freed blocks are reusable.
 * The small runtime-injected window is reserved even when a port relocates it
 * into this domain, and loaded ELF data/BSS remain owned by the image.
 * Use 64-bit arithmetic for every exclusive range end. */
static uint32_t user_address(uint32_t size, uint32_t alignment)
{
    uint64_t candidate = SYS_MEM_ALLOC_BASE;
    for (;;) {
        candidate = (candidate + alignment - 1u) & ~(uint64_t)(alignment - 1u);
        if (candidate + size > SYS_MEM_ALLOC_END) return 0;
        uint64_t next = s_image_query ? s_image_query(candidate, candidate + size) : candidate;
        uint64_t injected = VM_HLE_INJECT_BASE;
        if (candidate < injected + VM_HLE_INJECT_SIZE && injected < candidate + size &&
            injected + VM_HLE_INJECT_SIZE > next)
            next = injected + VM_HLE_INJECT_SIZE;
        if (candidate < (uint64_t)VM_PPU_TLS_BASE + VM_PPU_TLS_SIZE &&
            VM_PPU_TLS_BASE < candidate + size && (uint64_t)VM_PPU_TLS_BASE + VM_PPU_TLS_SIZE > next)
            next = (uint64_t)VM_PPU_TLS_BASE + VM_PPU_TLS_SIZE;
        for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; ++i) {
            const sys_mem_alloc_info* a = &g_sys_mem_allocs[i];
            uint64_t end = (uint64_t)a->addr + a->size;
            if (a->active && candidate < end && a->addr < candidate + size && end > next)
                next = end;
        }
        if (next == candidate) return (uint32_t)candidate;
        candidate = next;
    }
}

/* sys_memory_allocate and sys_memory_allocate_from_container share this. cid 0
 * is the process's main pool; a container's allocations are also charged to
 * that container (its used_size), checked against its total. */
static int64_t memory_allocate(uint64_t requested, uint32_t flags, uint32_t cid, uint32_t addr_out)
{
    uint32_t alignment = (flags & SYS_MEMORY_PAGE_SIZE_1M) ? 0x100000u : 0x10000u;
    uint64_t rounded = (requested + alignment - 1u) & ~(uint64_t)(alignment - 1u);
    if (!requested || requested > UINT32_MAX || !rounded || rounded > UINT32_MAX)
        return (int64_t)(int32_t)CELL_EINVAL;
    uint32_t size = (uint32_t)rounded;
    bump_lock();
    if (size > SYS_MEM_USER_TOTAL - s_total_allocated) {
        bump_unlock(); return (int64_t)(int32_t)CELL_ENOMEM;
    }
    sys_mem_container_info* ct = NULL;
    if (cid) {
        ct = &g_sys_mem_containers[cid - 1];
        if (size > ct->total_size - ct->used_size) {
            bump_unlock(); return (int64_t)(int32_t)CELL_ENOMEM;
        }
    }
    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; ++i)
        if (!g_sys_mem_allocs[i].active) { slot = i; break; }
    uint32_t addr = slot < 0 ? 0 : user_address(size, alignment);
    if (!addr || vm_commit(addr, size) != CELL_OK) {
        bump_unlock(); return (int64_t)(int32_t)CELL_ENOMEM;
    }
    memset(vm_to_host(addr), 0, size);
    sys_mem_alloc_info* a = &g_sys_mem_allocs[slot];
    a->active = 1; a->addr = addr; a->size = size;
    a->container_id = (int32_t)cid; a->page_size = alignment;
    s_total_allocated += size;
    if (ct) ct->used_size += size;
    if (addr + size > g_sys_mem_bump_ptr) g_sys_mem_bump_ptr = addr + size;
    bump_unlock();
    fprintf(stderr, "[sys_memory] allocate(size=0x%X, flags=0x%X, cid=%u) -> 0x%08X\n", size, flags, cid, addr);
    if (addr_out) write_be32(addr_out, addr);
    return CELL_OK;
}

int64_t sys_memory_allocate(ppu_context* ctx)
{
    return memory_allocate(LV2_ARG_U64(ctx, 0), LV2_ARG_U32(ctx, 1), 0, LV2_ARG_PTR(ctx, 2));
}

/* ---------------------------------------------------------------------------
 * sys_memory_allocate_from_container (syscall 350)
 *
 * r3 = size, r4 = container id, r5 = flags, r6 = pointer to receive the address
 *
 * Was unregistered. MLB The Show 16 builds its animation and movie buffer pools
 * ("SharedMemContAnim", "SharedMemContMovie") in containers carved from the
 * default one; with no allocation the pool stayed empty and loading a match
 * called a method on a NULL buffer.
 * -----------------------------------------------------------------------*/
int64_t sys_memory_allocate_from_container(ppu_context* ctx)
{
    uint32_t cid = LV2_ARG_U32(ctx, 1);
    if (cid == SYS_MEMORY_CONTAINER_DEFAULT)
        return memory_allocate(LV2_ARG_U64(ctx, 0), LV2_ARG_U32(ctx, 2), 0, LV2_ARG_PTR(ctx, 3));
    if (cid == 0 || cid > SYS_MEMORY_CONTAINER_MAX || !g_sys_mem_containers[cid - 1].active)
        return (int64_t)(int32_t)CELL_ESRCH;
    return memory_allocate(LV2_ARG_U64(ctx, 0), LV2_ARG_U32(ctx, 2), cid, LV2_ARG_PTR(ctx, 3));
}

/* ---------------------------------------------------------------------------
 * sys_memory_free
 *
 * r3 = address
 * -----------------------------------------------------------------------*/
int64_t sys_memory_free(ppu_context* ctx)
{
    uint32_t addr = LV2_ARG_U32(ctx, 0);
    bump_lock();
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++) {
        if (g_sys_mem_allocs[i].active && g_sys_mem_allocs[i].addr == addr) {
            s_total_allocated -= g_sys_mem_allocs[i].size;
            const int32_t cid = g_sys_mem_allocs[i].container_id;
            if (cid > 0 && cid <= SYS_MEMORY_CONTAINER_MAX && g_sys_mem_containers[cid - 1].active)
                g_sys_mem_containers[cid - 1].used_size -= g_sys_mem_allocs[i].size;
            g_sys_mem_allocs[i].active = 0;
            bump_unlock(); return CELL_OK;
        }
    }
    bump_unlock();
    return (int64_t)(int32_t)CELL_EINVAL;
}

/* ---------------------------------------------------------------------------
 * sys_memory_get_user_memory_size
 *
 * r3 = pointer to output struct: { u32 total, u32 available }
 * -----------------------------------------------------------------------*/
int64_t sys_memory_get_user_memory_size(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);

    fprintf(stderr, "[sys_memory] get_user_memory_size()\n");
    if (out_addr != 0) {
        uint32_t total = SYS_MEM_USER_TOTAL;
        uint32_t avail = total - s_total_allocated;
        write_be32(out_addr + 0, total);
        write_be32(out_addr + 4, avail);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_get_page_attribute (syscall 351 / 0x15F)
 *
 * r3 = address to query
 * r4 = pointer to sys_page_attribute_t:
 *        +0x00 attribute   (u64)
 *        +0x08 access_right(u64)
 *        +0x10 page_size   (u32)   <- the field titles actually check
 *        +0x14 pad         (u32)
 *
 * Was unimplemented (fell through to the return-0 stub), so the output struct
 * was never filled -> callers reading page_size got stack garbage. Dantelion2's
 * graphics init (func_009F6D40) queries a 1MB-page buffer and only proceeds to
 * construct a sub-object when page_size == 0x100000; the stub made it take the
 * failure path, leaving that sub-object NULL -> later null-deref crash.
 * -----------------------------------------------------------------------*/
int64_t sys_memory_get_page_attribute(ppu_context* ctx)
{
    uint32_t addr     = LV2_ARG_U32(ctx, 0);
    uint32_t attr_out = LV2_ARG_PTR(ctx, 1);

    /* Report the page size of the allocation containing addr. */
    uint32_t page_size = 0x10000;  /* default 64K */
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++) {
        if (g_sys_mem_allocs[i].active &&
            addr >= g_sys_mem_allocs[i].addr &&
            addr <  g_sys_mem_allocs[i].addr + g_sys_mem_allocs[i].size) {
            page_size = g_sys_mem_allocs[i].page_size ? g_sys_mem_allocs[i].page_size : 0x10000;
            break;
        }
    }

    if (attr_out != 0) {
        write_be32(attr_out + 0x00, 0);          /* attribute    (hi) */
        write_be32(attr_out + 0x04, 0x00040000); /* attribute    (lo) PROT_READ_WRITE-ish */
        write_be32(attr_out + 0x08, 0);          /* access_right (hi) */
        write_be32(attr_out + 0x0C, 0x00000008); /* access_right (lo) PPU_THREAD-ish */
        write_be32(attr_out + 0x10, page_size);  /* page_size */
        write_be32(attr_out + 0x14, 0);          /* pad */
    }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_create
 *
 * r3 = pointer to receive container ID (u32*)
 * r4 = size
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t size        = LV2_ARG_U32(ctx, 1);

    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_CONTAINER_MAX; i++) {
        if (!g_sys_mem_containers[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_mem_container_info* c = &g_sys_mem_containers[slot];
    c->active     = 1;
    c->total_size = size;
    c->used_size  = 0;

    uint32_t container_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, container_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_destroy
 *
 * r3 = container_id
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_destroy(ppu_context* ctx)
{
    uint32_t container_id = LV2_ARG_U32(ctx, 0);

    if (container_id == 0 || container_id > SYS_MEMORY_CONTAINER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mem_container_info* c = &g_sys_mem_containers[container_id - 1];
    if (!c->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    c->active = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_get_size
 *
 * r3 = pointer to output struct: sys_memory_info_t { u32 total, u32 available }
 * r4 = container_id; SYS_MEMORY_CONTAINER_ID_INVALID (0xFFFFFFFF) means the
 *      process's default container, i.e. the title's own user memory.
 *
 * The argument order and the second field were both wrong: the game passes the
 * struct first, and the second word is what is still free, not what is used. A
 * title carving a container out of the default one ("get the size, destroy,
 * recreate smaller, create mine") got ESRCH, then a container id of 0.
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_get_size(ppu_context* ctx)
{
    uint32_t out_addr     = LV2_ARG_PTR(ctx, 0);
    uint32_t container_id = LV2_ARG_U32(ctx, 1);

    if (container_id == SYS_MEMORY_CONTAINER_DEFAULT) {
        if (out_addr != 0) {
            bump_lock();
            const uint32_t avail = SYS_MEM_USER_TOTAL - s_total_allocated;
            bump_unlock();
            write_be32(out_addr + 0, SYS_MEM_USER_TOTAL);
            write_be32(out_addr + 4, avail);
        }
        return CELL_OK;
    }
    if (container_id == 0 || container_id > SYS_MEMORY_CONTAINER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mem_container_info* c = &g_sys_mem_containers[container_id - 1];
    if (!c->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (out_addr != 0) {
        write_be32(out_addr + 0, c->total_size);
        write_be32(out_addr + 4, c->total_size - c->used_size);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_address
 *
 * r3 = size
 * r4 = flags
 * r5 = alignment
 * r6 = pointer to receive address (u32*)
 * -----------------------------------------------------------------------*/
/* Mapper reservation and legacy direct mapping share their own cursor.
 * Called with the allocator lock held. */
static uint32_t mapper_address(uint64_t requested, uint64_t alignment)
{
    if (!requested || requested > UINT32_MAX || !alignment ||
        alignment > UINT32_MAX || (alignment & (alignment - 1u))) return 0;
    uint64_t addr = ((uint64_t)s_mapper_bump_ptr + alignment - 1u) & ~(alignment - 1u);
    uint64_t size = (requested + alignment - 1u) & ~(alignment - 1u);
    /* A reservation owns its entire aligned span. Recheck after every skip:
     * an alignment adjustment can move it into another runtime-owned window. */
    const uint64_t reserved[][2] = {
        { VM_HLE_HEAP_BASE, (uint64_t)VM_HLE_HEAP_BASE + VM_HLE_HEAP_SIZE },
        { VM_AUDIO_BASE, (uint64_t)VM_AUDIO_BASE + VM_AUDIO_SIZE },
        { VM_SYS_VM_BASE, (uint64_t)VM_SYS_VM_BASE + VM_SYS_VM_SIZE },
        { VM_HLE_INJECT_BASE, (uint64_t)VM_HLE_INJECT_BASE + VM_HLE_INJECT_SIZE }
    };
    for (;;) {
        uint64_t next = s_image_query ? s_image_query(addr, addr + size) : addr;
        for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
            if (addr < reserved[i][1] && reserved[i][0] < addr + size &&
                reserved[i][1] > next) next = reserved[i][1];
        if (next == addr) break;
        addr = (next + alignment - 1u) & ~(alignment - 1u);
    }
    if (!size || addr + size > SYS_MAPPER_END || vm_commit((uint32_t)addr, (uint32_t)size) != CELL_OK)
        return 0;
    s_mapper_bump_ptr = (uint32_t)(addr + size);
    return (uint32_t)addr;
}

int64_t sys_mmapper_allocate_address(ppu_context* ctx)
{
    uint64_t size = LV2_ARG_U64(ctx, 0);
    uint64_t alignment = LV2_ARG_U64(ctx, 2);
    uint32_t out = LV2_ARG_PTR(ctx, 3);
    if (!alignment) alignment = 0x10000u;
    if (alignment > UINT32_MAX || (alignment & (alignment - 1u)) || !size || size > UINT32_MAX)
        return (int64_t)(int32_t)CELL_EINVAL;
    bump_lock();
    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; ++i)
        if (!s_mapper_reservations[i].start) { slot = i; break; }
    uint32_t addr = slot < 0 ? 0 : mapper_address(size, alignment);
    if (addr) {
        mapper_reservation* r = &s_mapper_reservations[slot];
        r->start = r->next = addr; r->end = s_mapper_bump_ptr;
    }
    bump_unlock();
    if (!addr) return (int64_t)(int32_t)CELL_ENOMEM;
    if (out) write_be32(out, addr);
    return CELL_OK;
}

/* Claim a mapped block inside the caller's existing reservation. The loader's
 * search-and-map bridge and the allocator share this ownership bookkeeping,
 * so an unsuccessful request leaves all reservation cursors unchanged. */
int64_t sys_mmapper_search_map_address(uint32_t start, uint32_t size, uint32_t* out)
{
    if (!out || !size) return (int64_t)(int32_t)CELL_EINVAL;
    bump_lock();
    mapper_reservation* found = NULL;
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; ++i) {
        mapper_reservation* r = &s_mapper_reservations[i];
        if (r->start && ((!start && !found) || (start >= r->start && start < r->end))) {
            found = r; if (start) break;
        }
    }
    if (!found) {
        bump_unlock(); return (int64_t)(int32_t)CELL_EINVAL;
    }
    uint64_t addr = found->next;
    if (start > addr) addr = start;
    addr = (addr + 0xFFFFFu) & ~0xFFFFFull;
    uint64_t end = (addr + size + 0xFFFFFu) & ~0xFFFFFull;
    if (end > found->end || vm_commit((uint32_t)addr, size) != CELL_OK) {
        bump_unlock(); return (int64_t)(int32_t)CELL_ENOMEM;
    }
    found->next = (uint32_t)end;
    *out = (uint32_t)addr;
    bump_unlock(); return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_free_address
 *
 * r3 = address
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_free_address(ppu_context* ctx)
{
    /* uint32_t addr = LV2_ARG_U32(ctx, 0); */
    (void)ctx;
    /* In our bump allocator we don't actually free, just acknowledge */
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_shared_memory
 *
 * r3 = key
 * r4 = size
 * r5 = flags
 * r6 = pointer to receive shared mem ID (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_allocate_shared_memory(ppu_context* ctx)
{
    uint64_t key      = LV2_ARG_U64(ctx, 0);
    uint32_t size     = LV2_ARG_U32(ctx, 1);
    /* uint32_t flags = LV2_ARG_U32(ctx, 2); */
    uint32_t id_out   = LV2_ARG_PTR(ctx, 3);

    int slot = -1;
    for (int i = 0; i < SYS_MMAPPER_SHARED_MAX; i++) {
        if (!g_sys_mmapper_shared[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_mmapper_shared_info* s = &g_sys_mmapper_shared[slot];
    s->active = 1;
    s->size   = VM_ALIGN_UP(size, VM_PAGE_SIZE);
    s->key    = key;
    s->addr   = 0;

    uint32_t shm_id = (uint32_t)(slot + 1);
    if (id_out != 0) {
        write_be32(id_out, shm_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_memory (lv2 341)
 *
 * r3 = pointer to receive the allocated address (u32*)
 * r4 = size
 *
 * The ABI here was read off the caller rather than a header. GT5P's PDI device
 * worker calls this the moment it starts -- func_00941808 loads r3 with a
 * pointer and r4 with 0x300000, issues the syscall, retries on any non-zero
 * result, and on success immediately does `lwz r29, 0(r29)`, reading back
 * through the pointer it passed in r3.
 *
 * Answering CELL_OK without writing anything is the worst of both worlds: the
 * worker believes it has three megabytes, reads a null buffer back, and exits
 * on the spot -- which leaves the packed-filesystem device with no worker at
 * all and stalls asset loading. Answering an error is worse still, because the
 * caller retries forever.
 *
 * The flat VM has no distinction between reserving and mapping, so this hands
 * back committed guest memory directly and sys_mmapper_allocate_memory_from_
 * container below accepts it as already mapped.
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_allocate_memory(ppu_context* ctx)
{
    uint32_t addr_out = LV2_ARG_PTR(ctx, 0);
    uint32_t size     = LV2_ARG_U32(ctx, 1);

    if (size == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    bump_lock();
    uint32_t addr = mapper_address(size, VM_PAGE_SIZE);
    bump_unlock();
    if (!addr) return (int64_t)(int32_t)CELL_ENOMEM;

    if (addr_out != 0)
        write_be32(addr_out, addr);

    { static int n = 0; if (PS3_LOG_FIRST(n, 4))
        fprintf(stderr, "[sys_memory] mmapper_allocate_memory size=0x%X -> 0x%08X\n",
                size, addr); }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_memory_from_container (lv2 342)
 *
 * The same allocation charged to a container. GT5P calls it straight after 341
 * with the address 341 handed back, so with a flat VM there is nothing further
 * to do -- the memory is already committed and addressable.
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_allocate_memory_from_container(ppu_context* ctx)
{
    (void)ctx;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_map_shared_memory
 *
 * r3 = address (where to map)
 * r4 = shared mem ID
 * r5 = flags
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_map_shared_memory(ppu_context* ctx)
{
    uint32_t addr   = LV2_ARG_U32(ctx, 0);
    uint32_t shm_id = LV2_ARG_U32(ctx, 1);
    /* uint32_t flags = LV2_ARG_U32(ctx, 2); */

    if (shm_id == 0 || shm_id > SYS_MMAPPER_SHARED_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mmapper_shared_info* s = &g_sys_mmapper_shared[shm_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Commit the memory at the specified address */
    int32_t rc = vm_commit(addr, s->size);
    if (rc != CELL_OK)
        return (int64_t)(int32_t)rc;

    s->addr = addr;
    memset(vm_to_host(addr), 0, s->size);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_memory_init(lv2_syscall_table* tbl)
{
    bump_lock();
    memset(g_sys_mem_allocs,     0, sizeof(g_sys_mem_allocs));
    memset(g_sys_mem_containers, 0, sizeof(g_sys_mem_containers));
    memset(g_sys_mmapper_shared, 0, sizeof(g_sys_mmapper_shared));
    g_sys_mem_bump_ptr = 0;
    s_total_allocated  = 0;
    s_mapper_bump_ptr = SYS_MAPPER_BASE;
    memset(s_mapper_reservations, 0, sizeof(s_mapper_reservations));
    bump_unlock();

    lv2_syscall_register(tbl, SYS_MEMORY_ALLOCATE,             sys_memory_allocate);
    lv2_syscall_register(tbl, SYS_MEMORY_FREE,                 sys_memory_free);
    lv2_syscall_register(tbl, SYS_MEMORY_GET_USER_MEMORY_SIZE, sys_memory_get_user_memory_size);
    lv2_syscall_register(tbl, SYS_MEMORY_GET_PAGE_ATTRIBUTE,   sys_memory_get_page_attribute);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_CREATE,     sys_memory_container_create);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_DESTROY,    sys_memory_container_destroy);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_GET_SIZE,   sys_memory_container_get_size);
    lv2_syscall_register(tbl, SYS_MEMORY_ALLOCATE_FROM_CONTAINER, sys_memory_allocate_from_container);
    lv2_syscall_register(tbl, SYS_MMAPPER_ALLOCATE_ADDRESS,    sys_mmapper_allocate_address);
    lv2_syscall_register(tbl, SYS_MMAPPER_FREE_ADDRESS,        sys_mmapper_free_address);
    lv2_syscall_register(tbl, SYS_MMAPPER_ALLOCATE_SHARED_MEMORY, sys_mmapper_allocate_shared_memory);
    lv2_syscall_register(tbl, SYS_MMAPPER_MAP_SHARED_MEMORY,  sys_mmapper_map_shared_memory);
    lv2_syscall_register(tbl, 341, sys_mmapper_allocate_memory);
    lv2_syscall_register(tbl, 342, sys_mmapper_allocate_memory_from_container);
}

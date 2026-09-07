/*
 * ps3recomp - Virtual memory manager
 *
 * Manages the host-side mapping of the PS3's 32-bit address space.
 * On Windows uses VirtualAlloc; on POSIX uses mmap.
 *
 * PS3 memory map (simplified):
 *   0x00010000 - 0x0FFFFFFF : User-space (main memory, 256 MB)
 *   0x10000000 - 0x1FFFFFFF : RSX mapped memory
 *   0x30000000 - 0x3FFFFFFF : SPU local store windows (raw SPU mapping)
 *   0xD0000000 - 0xDFFFFFFF : Stack area
 */

#ifndef VM_H
#define VM_H

#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <signal.h>
  #include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Address space constants
 * -----------------------------------------------------------------------*/
#define VM_TOTAL_SIZE       0x100000000ull   /* 4 GB virtual address space */
#define VM_MAIN_MEM_BASE    0x00010000u
#define VM_MAIN_MEM_SIZE    0x10000000u      /* 256 MB main memory */
#define VM_RSX_BASE         0x10000000u
#define VM_RSX_SIZE         0x10000000u      /* 256 MB RSX */
#define VM_SPU_BASE         0x30000000u
#define VM_SPU_WINDOW_SIZE  0x00040000u      /* 256 KB per SPU LS */
#define VM_STACK_BASE       0xD0000000u
#define VM_STACK_REGION     0x10000000u      /* 256 MB stack region */

#define VM_PPU_STACK_SIZE   0x00100000u      /* 1 MB default PPU stack */

/* Home for guest-visible structures the RUNTIME injects -- the GCM label window,
 * the control register (put/get/ref) and the IO<->EA offset tables. These must
 * NOT sit inside main memory, because that is where the title's own allocator
 * hands out blocks: flOw's heap runs 0x00C00000..0x0D000000, which swallowed the
 * old 0x03000000 home. The game memset straight over the control register, our
 * FIFO drain then read ASCII text as `put` (0x3D3D3D0F -- "===" from a log
 * banner), walked `get` into unmapped IO and resynchronised away whole frames of
 * commands. Nothing was drawn and nothing said why.
 *
 * 0x20000000 is unclaimed by the map above: past RSX (ends 0x20000000) and well
 * below the raw-SPU windows (0x30000000). The title only ever learns these
 * addresses through cellGcmGetLabelAddress / cellGcmGetControlRegister, so the
 * value is free to move as long as every user goes through this base. */
/* No fixed value works for every title. 0x03000000 was the original home until
 * flOw's heap grew over it; 0x20000000 replaced it and Gran Turismo 5 Prologue
 * maps its own 174 MB heap there (cellGcmMapMainMemory(0x20000000, 0xAE00000)),
 * so the GCM window and its 16 KB of offset tables landed on the game's own
 * allocations and quietly destroyed them. The same failure, one address along.
 *
 * So it is a variable, not a constant: a port that knows its title's memory map
 * assigns ppu_hle_inject_base before it runs any guest code, and everything
 * derived from VM_HLE_INJECT_BASE follows. The default is the historical value.
 * The window needs 0x8000 bytes and the title only ever learns these addresses
 * through cellGcmGetLabelAddress / cellGcmGetControlRegister /
 * cellGcmGetOffsetTable, so it is free to move. */
#ifdef __cplusplus
extern "C" {
#endif
extern uint32_t ppu_hle_inject_base;
#ifdef __cplusplus
}
#endif
#define VM_HLE_INJECT_BASE  ppu_hle_inject_base

#define VM_PAGE_SIZE        0x00001000u      /* 4 KB page size */

/* Align a value up to `align` (must be power of 2) */
#define VM_ALIGN_UP(val, align) (((val) + (align) - 1) & ~((align) - 1))

/* ---------------------------------------------------------------------------
 * Global base pointer
 *
 * All PS3 guest address translation goes through this:
 *   host_ptr = vm_base + guest_addr
 * -----------------------------------------------------------------------*/
extern uint8_t* vm_base;

/* Guest address-space size; set non-zero once the host has mapped guest memory
 * (ppu_loader.cpp). Under native-VA mapping vm_base is deliberately 0 (guest
 * addr == host addr), so "is the VM ready?" must test this, not vm_base. */
extern uint32_t ppu_vm_size;
#define VM_READY (vm_base != 0 || ppu_vm_size != 0)

/* ---------------------------------------------------------------------------
 * Initialization / Shutdown
 * -----------------------------------------------------------------------*/

static inline void vm_shutdown(void);

/*
 * Reserve the host address range and commit the main memory region.
 * Returns CELL_OK on success, CELL_ENOMEM on failure.
 *
 * The base is 4 GB aligned, and that is a guarantee rather than a hope.
 * Translation runs one way as `host = vm_base + guest`, but a great deal of
 * the HLE runs the other way: a bridge handed a host pointer recovers the
 * guest address by truncating the pointer to 32 bits. GUEST_EA in
 * libs/guest_struct.h is that, and the GUEST_PTR macros accept either kind of
 * value on the same reasoning. The identity holds only while the low 32 bits
 * of vm_base are zero.
 *
 * It used to hold by luck, because both kernels tend to place a 4 GB
 * reservation on a 4 GB boundary. Nothing promises that, and the failure when
 * it stops is not a crash: the truncated pointer is a plausible-looking guest
 * address that moves with every run, so a write lands somewhere in the guest
 * arena and the guest's own variable keeps whatever was in it. The cost of
 * removing the question is one over-reservation that is trimmed back.
 */
static inline int32_t vm_init(void)
{
    if (vm_base != NULL) return CELL_OK; /* already initialized */

#ifdef _WIN32
    /*
     * Reserve a contiguous 4 GB region.  We only commit pages as needed.
     * MEM_RESERVE just reserves address space without backing pages.
     *
     * Windows cannot release part of a reservation, so the equivalent of the
     * POSIX trim below is to over-reserve, note where the aligned base falls,
     * release the lot and re-reserve exactly there. Another allocation could
     * take that address in between, hence the retries; on a 64-bit address
     * space losing a race for an 8 GB hole repeatedly is not a real risk, but
     * failing outright beats carrying on unaligned.
     */
    for (int attempt = 0; attempt < 8 && !vm_base; attempt++) {
        uint8_t* probe = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)(VM_TOTAL_SIZE * 2),
                                                MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) break;
        uint8_t* aligned = (uint8_t*)(((uintptr_t)probe + (uintptr_t)VM_TOTAL_SIZE - 1u)
                                      & ~((uintptr_t)VM_TOTAL_SIZE - 1u));
        VirtualFree(probe, 0, MEM_RELEASE);
        vm_base = (uint8_t*)VirtualAlloc(aligned, (SIZE_T)VM_TOTAL_SIZE,
                                         MEM_RESERVE, PAGE_NOACCESS);
    }
    if (!vm_base) return CELL_ENOMEM;

    /* Commit main memory region (256 MB, read/write) */
    if (!VirtualAlloc(vm_base + VM_MAIN_MEM_BASE, VM_MAIN_MEM_SIZE,
                      MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(vm_base, 0, MEM_RELEASE);
        vm_base = NULL;
        return CELL_ENOMEM;
    }

    /* Commit stack region */
    if (!VirtualAlloc(vm_base + VM_STACK_BASE, VM_STACK_REGION,
                      MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(vm_base, 0, MEM_RELEASE);
        vm_base = NULL;
        return CELL_ENOMEM;
    }

#else /* POSIX */
    /* Over-map and trim: ask for twice the space, keep the 4 GB aligned window
     * inside it, and give the head and tail back. mmap can hand back part of a
     * mapping, so this needs no retry and cannot race. */
    {
        size_t want = (size_t)VM_TOTAL_SIZE;
        uint8_t* raw = (uint8_t*)mmap(NULL, want * 2, PROT_NONE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                      -1, 0);
        if (raw == MAP_FAILED) {
            vm_base = NULL;
            return CELL_ENOMEM;
        }
        uint8_t* aligned = (uint8_t*)(((uintptr_t)raw + (uintptr_t)want - 1u)
                                      & ~((uintptr_t)want - 1u));
        if (aligned > raw)
            munmap(raw, (size_t)(aligned - raw));
        if (raw + want * 2 > aligned + want)
            munmap(aligned + want, (size_t)((raw + want * 2) - (aligned + want)));
        vm_base = aligned;
    }

    /* Make main memory readable/writable */
    if (mprotect(vm_base + VM_MAIN_MEM_BASE, VM_MAIN_MEM_SIZE,
                 PROT_READ | PROT_WRITE) != 0) {
        munmap(vm_base, (size_t)VM_TOTAL_SIZE);
        vm_base = NULL;
        return CELL_ENOMEM;
    }

    /* Make stack region readable/writable */
    if (mprotect(vm_base + VM_STACK_BASE, VM_STACK_REGION,
                 PROT_READ | PROT_WRITE) != 0) {
        munmap(vm_base, (size_t)VM_TOTAL_SIZE);
        vm_base = NULL;
        return CELL_ENOMEM;
    }
#endif

    /* The invariant every host-pointer-to-guest-address truncation rests on.
     * The reservations above hold it by construction; this is here so that a
     * change to either of them announces itself now, instead of as a stray
     * four-byte write to a moving address somewhere else entirely. */
    assert(((uintptr_t)vm_base & ((uintptr_t)VM_TOTAL_SIZE - 1u)) == 0);
    if (((uintptr_t)vm_base & ((uintptr_t)VM_TOTAL_SIZE - 1u)) != 0) {
        vm_shutdown();
        return CELL_ENOMEM;
    }

    /* Zero main memory */
    memset(vm_base + VM_MAIN_MEM_BASE, 0, VM_MAIN_MEM_SIZE);

    return CELL_OK;
}

static inline void vm_shutdown(void)
{
    if (!vm_base) return;

#ifdef _WIN32
    VirtualFree(vm_base, 0, MEM_RELEASE);
#else
    munmap(vm_base, (size_t)VM_TOTAL_SIZE);
#endif

    vm_base = NULL;
}

/* ---------------------------------------------------------------------------
 * Memory region commit / protect
 * -----------------------------------------------------------------------*/

static inline int32_t vm_commit(uint32_t addr, uint32_t size)
{
    if (!VM_READY) return CELL_EFAULT;
    size = VM_ALIGN_UP(size, VM_PAGE_SIZE);

#ifdef _WIN32
    if (!VirtualAlloc(vm_base + addr, size, MEM_COMMIT, PAGE_READWRITE))
        return CELL_ENOMEM;
#else
    if (mprotect(vm_base + addr, size, PROT_READ | PROT_WRITE) != 0)
        return CELL_ENOMEM;
#endif

    return CELL_OK;
}

static inline int32_t vm_protect(uint32_t addr, uint32_t size, int read, int write, int exec)
{
    if (!VM_READY) return CELL_EFAULT;
    size = VM_ALIGN_UP(size, VM_PAGE_SIZE);

#ifdef _WIN32
    DWORD prot = PAGE_NOACCESS;
    if (read && write && exec) prot = PAGE_EXECUTE_READWRITE;
    else if (read && write)    prot = PAGE_READWRITE;
    else if (read && exec)     prot = PAGE_EXECUTE_READ;
    else if (read)             prot = PAGE_READONLY;
    else if (exec)             prot = PAGE_EXECUTE;

    DWORD old;
    if (!VirtualProtect(vm_base + addr, size, prot, &old))
        return CELL_EFAULT;
#else
    int prot = PROT_NONE;
    if (read)  prot |= PROT_READ;
    if (write) prot |= PROT_WRITE;
    if (exec)  prot |= PROT_EXEC;

    if (mprotect(vm_base + addr, size, prot) != 0)
        return CELL_EFAULT;
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Stack allocation for PPU threads
 *
 * Simple bump allocator within the stack region.  Each thread gets
 * a fixed-size stack with a guard page at the bottom.
 * -----------------------------------------------------------------------*/
typedef struct vm_stack_alloc {
    uint32_t next_addr;     /* next stack base to allocate */
    uint32_t region_end;    /* end of the stack region */
} vm_stack_alloc;

static inline void vm_stack_alloc_init(vm_stack_alloc* sa)
{
    sa->next_addr  = VM_STACK_BASE;
    sa->region_end = VM_STACK_BASE + VM_STACK_REGION;
}

/*
 * Allocate a stack.  Returns the BASE address of the stack region (lowest address).
 * The usable stack top = base + size.
 * Returns 0 on failure.
 */
static inline uint32_t vm_stack_allocate(vm_stack_alloc* sa, uint32_t stack_size)
{
    stack_size = VM_ALIGN_UP(stack_size, VM_PAGE_SIZE);

    /* Add a guard page */
    uint32_t total = stack_size + VM_PAGE_SIZE;

    if (sa->next_addr + total > sa->region_end)
        return 0; /* out of stack space */

    uint32_t base = sa->next_addr;
    sa->next_addr += total;

    /* Guard page at the bottom (no access) */
    vm_protect(base, VM_PAGE_SIZE, 0, 0, 0);

    return base + VM_PAGE_SIZE; /* skip the guard page */
}

/* ---------------------------------------------------------------------------
 * SPU local store allocation
 *
 * Each SPU gets a 256 KB window in the VM_SPU_BASE range, used for
 * raw SPU memory-mapped access from the PPU.
 * -----------------------------------------------------------------------*/

static inline int32_t vm_spu_ls_map(uint32_t spu_index)
{
    if (spu_index >= 8) return CELL_EINVAL; /* max 8 SPUs */

    uint32_t addr = VM_SPU_BASE + spu_index * VM_SPU_WINDOW_SIZE;
    return vm_commit(addr, VM_SPU_WINDOW_SIZE);
}

static inline uint32_t vm_spu_ls_addr(uint32_t spu_index)
{
    return VM_SPU_BASE + spu_index * VM_SPU_WINDOW_SIZE;
}

static inline uint8_t* vm_spu_ls_host_ptr(uint32_t spu_index)
{
    return vm_base + VM_SPU_BASE + spu_index * VM_SPU_WINDOW_SIZE;
}

/* ---------------------------------------------------------------------------
 * Address translation helpers
 * -----------------------------------------------------------------------*/

static inline void* vm_to_host(uint32_t addr)
{
    return (void*)(vm_base + addr);
}

static inline uint32_t vm_to_guest(const void* host_ptr)
{
    return (uint32_t)((const uint8_t*)host_ptr - vm_base);
}

static inline int vm_is_valid_addr(uint32_t addr)
{
    /* Check if address falls within a committed region */
    if (addr >= VM_MAIN_MEM_BASE && addr < VM_MAIN_MEM_BASE + VM_MAIN_MEM_SIZE)
        return 1;
    if (addr >= VM_STACK_BASE && addr < VM_STACK_BASE + VM_STACK_REGION)
        return 1;
    if (addr >= VM_SPU_BASE && addr < VM_SPU_BASE + 8 * VM_SPU_WINDOW_SIZE)
        return 1;
    if (addr >= VM_RSX_BASE && addr < VM_RSX_BASE + VM_RSX_SIZE)
        return 1;
    return 0;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VM_H */

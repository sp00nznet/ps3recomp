/*
 * guest_arena.h -- the POSIX guest arena for the project template.
 *
 * Every guest address maps to vm_base + address, and a good deal of the HLE
 * runs that the other way: handed a host pointer, it recovers the guest
 * address by truncating the pointer to 32 bits (GUEST_EA in
 * libs/guest_struct.h). That only works while the low 32 bits of vm_base are
 * zero, which is why the toolkit's own vm_init() (runtime/memory/vm.h) maps
 * its arena on a 4 GB boundary. A plain mmap of the arena is page aligned but
 * lands on a 4 GB boundary only by luck, and when the luck runs out nothing
 * crashes: the truncated pointer is a plausible guest address that moves with
 * every run, so writes land somewhere else in guest memory.
 *
 * guest_arena_map() therefore over-maps, keeps the 4 GB aligned window inside
 * the mapping and gives the head and tail back, as vm_init() does. The `size`
 * bytes it returns are read/write and lazily backed: only the pages a title
 * touches cost memory. mmap can release part of a mapping, so this needs no
 * retry and cannot race another allocation.
 */
#ifndef PS3RECOMP_TEMPLATE_GUEST_ARENA_H
#define PS3RECOMP_TEMPLATE_GUEST_ARENA_H

#if !defined(_WIN32)
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

#define GUEST_ARENA_ALIGN 0x100000000ull   /* the 32-bit guest window */

/* Map `size` bytes of zeroed read/write memory whose base is 4 GB aligned.
 * Returns NULL on failure. Release with guest_arena_unmap(base, size). */
static inline uint8_t* guest_arena_map(uint64_t size)
{
    if (size == 0 || size > SIZE_MAX - GUEST_ARENA_ALIGN) return NULL;
    const size_t span = (size_t)(size + GUEST_ARENA_ALIGN);
    uint8_t* raw = (uint8_t*)mmap(NULL, span, PROT_NONE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) return NULL;
    uint8_t* base = (uint8_t*)(((uintptr_t)raw + (uintptr_t)(GUEST_ARENA_ALIGN - 1u))
                               & ~(uintptr_t)(GUEST_ARENA_ALIGN - 1u));
    if (base > raw)
        munmap(raw, (size_t)(base - raw));
    if (raw + span > base + size)
        munmap(base + size, (size_t)((raw + span) - (base + size)));
    if (mprotect(base, (size_t)size, PROT_READ | PROT_WRITE) != 0) {
        munmap(base, (size_t)size);
        return NULL;
    }
    return base;
}

static inline void guest_arena_unmap(uint8_t* base, uint64_t size)
{
    if (base) munmap(base, (size_t)size);
}
#endif /* !_WIN32 */

#endif /* PS3RECOMP_TEMPLATE_GUEST_ARENA_H */

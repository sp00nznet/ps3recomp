/* templates/project/guest_arena.h: the template's POSIX guest arena must sit
 * on a 4 GB boundary (the HLE truncates host pointers to recover guest
 * addresses), be read/write and zeroed across its whole size, and come back
 * aligned however the address space is already laid out. Synthetic: no game
 * files, nothing but anonymous mappings. */
#include "guest_arena.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARENA_SIZE 0x100010000ull   /* the template's VM_SIZE: 4 GB + a 64K guard */
#define HELD       4

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    uint8_t* arenas[HELD] = {0};
    void* spacers[HELD] = {0};
    /* Hold every arena at once, with an odd-sized mapping between them, so
     * the later ones cannot all land on a boundary by the kernel's habit. */
    for (int i = 0; i < HELD; i++) {
        spacers[i] = mmap(NULL, (size_t)0x10000u * (size_t)(i + 1), PROT_READ,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(spacers[i] != MAP_FAILED);
        arenas[i] = guest_arena_map(ARENA_SIZE);
        CHECK(arenas[i] != NULL);
        if (!arenas[i]) continue;
        CHECK(((uintptr_t)arenas[i] & 0xFFFFFFFFu) == 0);
        uint8_t* a = arenas[i];
        CHECK(a[0] == 0 && a[0xD0000000u] == 0 && a[ARENA_SIZE - 1] == 0);
        a[0] = 0x11; a[0xD0000000u] = 0x22; a[ARENA_SIZE - 1] = 0x33;
        CHECK(a[0] == 0x11 && a[0xD0000000u] == 0x22 && a[ARENA_SIZE - 1] == 0x33);
        /* A host pointer truncated to 32 bits is the guest address. */
        CHECK((uint32_t)(uintptr_t)(a + 0xD0000000u) == 0xD0000000u);
    }
    for (int i = 0; i < HELD; i++) {
        guest_arena_unmap(arenas[i], ARENA_SIZE);
        if (spacers[i] != MAP_FAILED && spacers[i]) munmap(spacers[i], (size_t)0x10000u * (size_t)(i + 1));
    }
    CHECK(guest_arena_map(0) == NULL);

    if (failures) { fprintf(stderr, "template guest arena: %d failure(s)\n", failures); return 1; }
    printf("template guest arena: %d arenas 4 GB aligned, read/write and zeroed\n", HELD);
    return 0;
}

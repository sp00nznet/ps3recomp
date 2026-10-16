/* Manufactured ELF data only; exercises the production loader and allocators. */
#include "../runtime/syscalls/sys_memory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include "win32_compat.h"

extern "C" {
uint8_t* vm_base;
uint32_t ppu_hle_inject_base = 0x20000000u;
uint32_t ppu_load_elf(const char*);
/* These subsystems are inactive in a loader-only fixture. */
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) { std::abort(); }
void spu_lockline_lock(void) { std::abort(); }
void spu_lockline_unlock(void) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
HANDLE CreateThread(void*, SIZE_T, PS3_THREAD_FN, LPVOID, DWORD, DWORD*) { std::abort(); }
BOOL CloseHandle(HANDLE) { std::abort(); }
DWORD GetFileAttributesA(const char*) { std::abort(); }
int64_t sys_mmapper_allocate_memory(ppu_context*);
}
struct func_entry { uint32_t addr; void (*func)(ppu_context*); const char* name; };
extern const func_entry function_table[] = {{0, nullptr, nullptr}};
extern const uint64_t function_table_count = 0;
static lv2_syscall_table table;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static uint32_t output()
{
    const uint8_t* p = vm_base + 0x10000;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
static int64_t allocate(uint32_t size)
{
    ppu_context c{};
    c.gpr[3] = size; c.gpr[4] = 0x400; c.gpr[5] = 0x10000;
    return sys_memory_allocate(&c);
}
int main(int argc, char** argv)
{
    CHECK(argc == 3);
    vm_base = (uint8_t*)mmap(nullptr, 0xE0000000ull, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(vm_base != MAP_FAILED);
    ppu_vm_size = 0xE0000000u;
    CHECK(ppu_load_elf(argv[1]) == 0x10000);
    /* Match boot_main: syscall initialization follows ELF loading. */
    sys_memory_init(&table);
    CHECK(!std::memcmp(vm_base + 0x10000000, "QRST", 4));
    CHECK(vm_base[0x1077FFFF] == 0);
    vm_base[0x1077FFFF] = 0xA5;  /* live object in the loaded segment's BSS */
    vm_base[0x4020FFFF] = 0x5A;  /* BSS-only mapper-domain segment */
    CHECK(allocate(0xA400000) == CELL_OK);
    ppu_context c{};
    c.gpr[3] = 0x10000; c.gpr[4] = 0x500000;
    CHECK(sys_mmapper_allocate_memory(&c) == CELL_OK);
    uint32_t mapped = output();
    CHECK(mapped >= 0x40210000u);
    std::memset(vm_base + mapped, 0xCC, 0x500000);
    CHECK(allocate(0x700000) == CELL_OK);
    CHECK(output() >= 0x10780000u);
    std::memset(vm_base + output(), 0xDD, 0x700000);
    CHECK(!std::memcmp(vm_base + 0x10000000, "QRST", 4));
    CHECK(!std::memcmp(vm_base + 0x10080000, "ABCD", 4));
    CHECK(vm_base[0x1077FFFF] == 0xA5);
    CHECK(!std::memcmp(vm_base + 0x40000000, "MAPS", 4));
    CHECK(vm_base[0x4020FFFF] == 0x5A);
    /* Reservation search must also avoid the image, including an aligned
     * candidate that lands in the second, BSS-only PT_LOAD. */
    sys_memory_init(&table);
    c = {};
    c.gpr[3] = 0x200000; c.gpr[5] = 0x100000; c.gpr[6] = 0x10000;
    CHECK(sys_mmapper_allocate_address(&c) == CELL_OK);
    uint32_t reservation = output();
    CHECK(reservation == 0x40300000u);
    uint32_t block = 0;
    CHECK(sys_mmapper_search_map_address(reservation, 0x100000, &block) == CELL_OK);
    CHECK(block == reservation);
    std::memset(vm_base + block, 0xEE, 0x100000);
    CHECK(vm_base[0x4020FFFF] == 0x5A);
    CHECK(!std::memcmp(vm_base + 0x40000000, "MAPS", 4));
    /* Replacing the executable replaces the range list, without stale holes. */
    CHECK(ppu_load_elf(argv[2]) == 0x10000);
    sys_memory_init(&table);
    CHECK(allocate(0x100000) == CELL_OK);
    CHECK(output() == 0x10000000u);
    c = {};
    c.gpr[3] = 0x10000; c.gpr[4] = 0x100000;
    CHECK(sys_mmapper_allocate_memory(&c) == CELL_OK);
    CHECK(output() == 0x40000000u);
    munmap(vm_base, 0xE0000000ull);
    std::printf("PASS %u loaded-image allocation checks\n", checks);
}

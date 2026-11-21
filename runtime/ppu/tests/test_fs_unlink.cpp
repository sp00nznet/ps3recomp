/* Synthetic stale-cache recovery through the actual PPU cellFs HLE. */
#include "ppu_fs.cpp"
#include "ps3emu/error_codes.h"
#include <filesystem>
#include <fstream>

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size;
/* No other I/O operation is exercised by this fixture. */
const char* edat_resolve(const char*, char*, size_t) { abort(); }
void vm_write32(uint64_t, uint32_t) { abort(); }
void vm_write64(uint64_t, uint64_t) { abort(); }
void ppu_guest_caller(char*, size_t) { abort(); }
void ydkj_host_bt(const char*) { abort(); }
void ps3_vfs_ps3game_fallback(char*, size_t) { abort(); }
ps3_guest_caller_fn g_ps3_guest_caller;
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
}

int main(int argc, char** argv)
{
    if (argc != 2) return 90;
    const auto root = std::filesystem::path(argv[1]);
#ifdef _WIN32
    _putenv_s("PS3_HDD1_ROOT", root.string().c_str());
#else
    setenv("PS3_HDD1_ROOT", root.string().c_str(), 1);
#endif
    std::filesystem::create_directories(root / "cache");
    const auto stale = root / "cache/stale.idx";
    std::ofstream(stale) << "synthetic invalid cache";
    const auto original = root / "original.dat";
    std::ofstream(original) << "preserve original";
    uint8_t memory[4096]{};
    vm_base = memory; ppu_vm_size = sizeof memory;
    strcpy((char*)memory + 256, "/dev_hdd1/cache/stale.idx");
    ppu_context ctx{};
    ctx.gpr[3] = 256; cellFsUnlink(&ctx);
    if (ctx.gpr[3] != CELL_OK || std::filesystem::exists(stale)) return 41;
    ctx.gpr[3] = 256; cellFsUnlink(&ctx);
    if ((int32_t)ctx.gpr[3] != (int32_t)CELL_ENOENT) return 42;
    ctx.gpr[3] = 0; cellFsUnlink(&ctx);
    if ((int32_t)ctx.gpr[3] != (int32_t)CELL_EINVAL) return 43;
    ctx.gpr[3] = sizeof memory; cellFsUnlink(&ctx);
    if ((int32_t)ctx.gpr[3] != (int32_t)CELL_EFAULT) return 45;
    strcpy((char*)memory + 256, "/dev_hdd1/cache");
    ctx.gpr[3] = 256; cellFsUnlink(&ctx);
    if (ctx.gpr[3] == CELL_OK || !std::filesystem::is_directory(root / "cache")) return 46;
    if (!std::filesystem::exists(original)) return 44;
    puts("PASS: stale cache removed; missing/null paths report errors; unrelated file preserved");
    return 0;
}

/*
 * ps3recomp game project -- entry point template
 *
 * Boots the ps3recomp runtime and hands control to the recompiled title:
 * reserve the guest address space, load the decrypted ELF into it, register
 * the lifted function table and the HLE/syscall layers, then run the ELF's
 * entry point.
 *
 * Every function called here is C, declared `extern "C"`, and lives in the
 * runtime. `lbp/main.cpp` in the ps3recomp tree is the fully worked example
 * this template is reduced from -- consult it for demand-committed memory,
 * crash handlers, SPU workload registration and PARAM.SFO handling.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

/* ---------------------------------------------------------------------------
 * Runtime entry points
 *
 * These are compiled into your game target from `runtime/ppu/`, not into
 * libps3recomp_runtime -- they #include the lifter-generated `ppu_recomp.h`,
 * which only exists once you have lifted a title. See docs/BUILDING.md,
 * "Graphics Backends" -> "The RSX code that is not in the library".
 * -----------------------------------------------------------------------*/
extern "C" {
    /* Guest address space. Point vm_base at a block big enough for the title
     * and set ppu_vm_size to its length (0 disables the out-of-bounds guard,
     * which is what ports do once the full 32-bit space is backed). */
    extern uint8_t* vm_base;
    extern uint32_t ppu_vm_size;

    /* Host directory that guest mount points (/dev_hdd0, /dev_bdvd, ...)
     * resolve into. */
    extern const char* ppu_vfs_root;

    uint32_t ppu_load_elf(const char* path);      /* -> entry OPD, 0 on failure */
    void     ppu_recomp_register(void);           /* generated: address -> function */
    void     ppu_hle_init(void);                  /* firmware NID -> HLE handler   */
    void     ppu_sysprx_register(void);           /* boot-critical CRT             */
    void     ppu_fs_register(void);               /* cellFs over ppu_vfs_root      */
    void     lv2_init_syscalls(void);             /* lv2 syscall table             */
    int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
}

/* Main-thread stack top, just below the 0x10000000 segment. */
#define STACK_TOP  0x0FF00000u

/* Guest space to back. The full 32-bit space is 4 GiB; committing it up front
 * is simple and portable, which is what a template should be. Titles that need
 * the whole range reserve it and commit on demand from a fault handler --
 * lbp/main.cpp shows that arrangement. */
#define VM_BYTES   0xE0000000ull

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: %s <EBOOT.elf> [vfs-root]\n", argv[0]);
        return 2;
    }

    vm_base = static_cast<uint8_t*>(std::calloc(1, VM_BYTES));
    if (!vm_base) {
        std::printf("[boot] could not allocate %llu bytes of guest memory\n",
                    static_cast<unsigned long long>(VM_BYTES));
        return 1;
    }
    ppu_vm_size = static_cast<uint32_t>(VM_BYTES);

    const uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        std::printf("[boot] failed to load %s\n", argv[1]);
        return 1;
    }

    /* Where the title's files live. Default to the ELF's own directory; a
     * second argument overrides it. */
    static char vfs[1024];
    if (argc > 2) {
        std::snprintf(vfs, sizeof vfs, "%s", argv[2]);
    } else {
        std::snprintf(vfs, sizeof vfs, "%s", argv[1]);
        char* sep = nullptr;
        for (char* p = vfs; *p; ++p)
            if (*p == 0x2F || *p == 0x5C) sep = p;
        if (sep) *sep = 0; else std::snprintf(vfs, sizeof vfs, ".");
    }
    ppu_vfs_root = vfs;
    std::printf("[boot] VFS root: %s\n", ppu_vfs_root);

    /* Order matters: the lifted table first, then the layers that resolve
     * against it. */
    ppu_recomp_register();
    ppu_hle_init();
    ppu_sysprx_register();
    ppu_fs_register();
    lv2_init_syscalls();

    std::printf("[boot] entry OPD 0x%08X, stack top 0x%08X\n", entry, STACK_TOP);
    const int rc = ppu_run(entry, STACK_TOP);
    std::printf("[boot] ppu_run returned %d\n", rc);
    return rc;
}

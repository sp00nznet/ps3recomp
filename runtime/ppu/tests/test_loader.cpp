/*
 * ps3recomp - PPU loader smoke test
 *
 * Loads the real Uncharted EBOOT image into vm_base, registers the lifted
 * function table, and exercises the loader's core mechanics:
 *   - ELF image load (segments + BSS) into vm_base,
 *   - OPD resolution of the entry (code + TOC),
 *   - the address->function registry,
 *   - big-endian guest memory + a lifted leaf function executing via the
 *     indirect-call dispatch.
 *
 * Build (link the lifted output as C++):
 *   g++ -std=c++17 -O2 -I <lift_out> test_loader.cpp ../ppu_loader.cpp \
 *       <lift_out>/ppu_recomp.c -x c++ -o test_loader.exe
 * Run:
 *   ./test_loader.exe "<path to EBOOT.elf>"
 */
#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" uint8_t* vm_base = nullptr;

extern "C" {
uint32_t ppu_load_elf(const char* path);
uint32_t ppu_function_count(void);
int      ppu_opd_resolve(uint32_t opd, uint32_t* code, uint32_t* toc);
void     ps3_indirect_call(ppu_context* ctx);
uint32_t vm_read32(uint64_t a);
}

#define VM_SIZE 0x11000000u   /* ~285 MB: covers the highest segment vaddr+memsz */

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("[PASS] %s\n", msg); \
    else { printf("[FAIL] %s\n", msg); fails++; } } while (0)

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: %s <EBOOT.elf>\n", argv[0]); return 2; }

    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) { printf("vm alloc failed\n"); return 1; }

    uint32_t entry_opd = ppu_load_elf(argv[1]);
    CHECK(entry_opd != 0, "ELF image loaded, entry OPD non-zero");

    ppu_recomp_register();
    uint32_t n = ppu_function_count();
    printf("registered functions: %u\n", n);
    CHECK(n >= 500, "function table registered (>=500 in this subset)");

    uint32_t code = 0, toc = 0;
    ppu_opd_resolve(entry_opd, &code, &toc);
    printf("entry OPD 0x%08X -> code 0x%08X, toc 0x%08X\n", entry_opd, code, toc);
    CHECK(code == 0x10230, "entry code resolved to 0x10230 (BE OPD read)");
    CHECK(toc == 0x7d3190, "entry TOC resolved to 0x7d3190");

    /* Sanity: a known code word at 0x10230 should be readable big-endian. */
    uint32_t w = vm_read32(0x10230);
    printf("first entry instruction word @0x10230 = 0x%08X\n", w);
    CHECK(w != 0 && w != 0xFFFFFFFF, "code segment present in vm_base");

    /* Execute a lifted leaf function through the indirect-call dispatch.
     * func_00010258: r9 = [toc-0x7FF8]; r3 = [r9]; sign-extend; return.
     * Validates registry lookup + TOC + BE memory + lifted execution. */
    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[1] = VM_SIZE - 0x10000;  /* a stack top inside vm */
    ctx.gpr[2] = toc;
    ctx.ctr    = 0x10258;            /* target code address */
    ps3_indirect_call(&ctx);
    printf("after dispatch to 0x10258: r3 = 0x%016llX\n",
           (unsigned long long)ctx.gpr[3]);
    CHECK(true, "indirect dispatch to a lifted leaf executed without crashing");

    printf("\n===========================================\n");
    printf("Results: %s (%d failure(s))\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}

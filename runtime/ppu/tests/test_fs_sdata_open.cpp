/*
 * ps3recomp - plain opens of SDATA and cellFsSdataOpen
 * (ppu_fs.cpp with the real libs/filesystem/edat.c).
 *
 * On hardware only cellFsSdataOpen decrypts an SDATA (.sdat) container; a plain
 * cellFsOpen returns the container's own bytes, the same ones cellFsStat sizes.
 * A title that copies its .sdat elsewhere must read exactly stat-size bytes. The decrypted copy that
 * cellFsSdataOpen serves must not be written into the (read-only) source tree.
 *
 * Everything here is synthetic: the SDATA is built by this file with edat.c's
 * own AES/CMAC primitives (NPD version 3, SDAT flag, three blocks, the last
 * partial) from made-up header bytes and plaintext. No key or file from any
 * title is used.
 *
 *   tools/test_ppu_fs_sdata_open.py
 *   test_fs_sdata_open DIR
 */
#include "ppu_fs.cpp"
#include "../../../libs/filesystem/edat.c"
/* ps3_dev_flash_path (runtime/platform), which host_path calls for /dev_flash. */
#include "../../platform/ps3_dev_flash.c"

/* ---- link stubs: nothing on the paths this test drives calls them ---- */
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
void ppu_guest_caller(char* out, size_t n) { if (n) out[0] = 0; }
void ps3_vfs_ps3game_fallback(char* path, size_t cap) { (void)path; (void)cap; }
void ydkj_host_bt(const char* tag) { (void)tag; }
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
}
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

#define BLOCK 0x400u
#define PLAIN 2500u   /* three blocks, the last 452 bytes */

static void put32(uint8_t* p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void cbc_encrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len)
{
    uint8_t rk[176], x[16];
    aes128_expand(key, rk);
    memcpy(x, iv, 16);
    for (size_t o = 0; o < len; o += 16) {
        for (int i = 0; i < 16; i++) x[i] ^= in[o + i];
        aes128_encrypt_block(rk, x, out + o);
        memcpy(x, out + o, 16);
    }
}

/* NPD v3 with SDAT_FLAG: file key = dev_hash ^ SDAT_KEY; one 16-byte CMAC per
 * block in a table at 0x100, then the blocks. Block key = AES(file key,
 * dev_hash[0..11] || be32(i)); data AES-CBC under it with the digest as IV.
 * Returns the container size. */
static long build_sdata(const char* path, const uint8_t* plain)
{
    const uint32_t nblk = (PLAIN + BLOCK - 1) / BLOCK;
    uint8_t hdr[0x100] = {0};
    memcpy(hdr, "NPD\0", 4);
    put32(hdr + 0x04, 3);
    put32(hdr + 0x08, 3);
    memcpy(hdr + 0x10, "XX0000-TEST00000_00-SYNTHETICSDAT0001", 36);
    for (int i = 0; i < 16; i++) hdr[0x40 + i] = (uint8_t)(0x30 + i);   /* digest */
    for (int i = 0; i < 16; i++) hdr[0x60 + i] = (uint8_t)(0xC0 + i);   /* dev_hash */
    put32(hdr + 0x80, SDAT_FLAG);
    put32(hdr + 0x84, BLOCK);
    put32(hdr + 0x88, 0); put32(hdr + 0x8C, PLAIN);
    uint8_t key[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(hdr[0x60 + i] ^ SDAT_KEY[i]);
    uint8_t rk[176];
    aes128_expand(key, rk);
    static uint8_t enc[3][BLOCK];
    uint8_t meta[3][16];
    long size = (long)sizeof hdr + 16L * nblk;
    for (uint32_t b = 0; b < nblk; b++) {
        uint32_t len = (b == nblk - 1 && PLAIN % BLOCK) ? PLAIN % BLOCK : BLOCK;
        uint32_t padded = (len + 15) & ~15u;
        uint8_t pt[BLOCK] = {0}, bk[16] = {0}, kres[16];
        memcpy(pt, plain + b * BLOCK, len);
        memcpy(bk, hdr + 0x60, 12);
        put32(bk + 12, b);
        aes128_encrypt_block(rk, bk, kres);
        cbc_encrypt(kres, hdr + 0x40, pt, enc[b], padded);
        aes128_cmac(kres, enc[b], padded, meta[b]);
        size += padded;
    }
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(hdr, 1, sizeof hdr, f);
    for (uint32_t b = 0; b < nblk; b++) fwrite(meta[b], 1, 16, f);
    for (uint32_t b = 0; b < nblk; b++) {
        uint32_t len = (b == nblk - 1 && PLAIN % BLOCK) ? PLAIN % BLOCK : BLOCK;
        fwrite(enc[b], 1, (len + 15) & ~15u, f);
    }
    return fclose(f) == 0 ? size : -1;
}

static int any_dec_beside(const char* dir)
{
    DIR* d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    while (struct dirent* e = readdir(d)) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strcmp(e->d_name + n - 4, ".dec") == 0) found = 1;
    }
    closedir(d);
    return found;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2) { puts("FAIL: usage: test_fs_sdata_open DIR"); return 1; }
    const char* dir = argv[1];
    static uint8_t mem[1 << 16];
    vm_base = mem; ppu_vm_size = sizeof mem;

    char disc[1100], cache[1100], host[1200];
    /* Without PS3_HDD1_ROOT, /dev_hdd1/<name> is <vfs root>/<name>. */
    snprintf(disc, sizeof disc, "%s", dir);
    snprintf(cache, sizeof cache, "%s/edat-cache", dir);
#ifdef _WIN32
    _putenv_s("PS3RECOMP_EDAT_CACHE", cache);
#else
    setenv("PS3RECOMP_EDAT_CACHE", cache, 1);
#endif
    snprintf(host, sizeof host, "%s/data.sdat", disc);
    static uint8_t plain[PLAIN];
    for (uint32_t i = 0; i < PLAIN; i++) plain[i] = (uint8_t)(i * 7 + (i >> 8));
    long size = build_sdata(host, plain);
    if (size < 0) { puts("FAIL: could not write the fixture"); return 1; }
    static uint8_t raw[8192];
    FILE* f = fopen(host, "rb");
    if (!f || (long)fread(raw, 1, sizeof raw, f) != size) { puts("FAIL: could not read the fixture back"); return 1; }
    fclose(f);

    ppu_vfs_root = dir;
    strcpy((char*)mem + 0x100, "/dev_hdd1/data.sdat");
    ppu_context ctx;

    CHECK(edat_is_sdata(host), "the fixture is an NPD container flagged SDATA");

    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = 0x400;
    cellFsStat(&ctx);
    CHECK((uint32_t)ctx.gpr[3] == 0 && rd64(0x400 + 0x24) == (uint64_t)size,
          "cellFsStat sizes the container: %llu (want %ld)", (unsigned long long)rd64(0x400 + 0x24), size);

    /* Plain open: the raw container, exactly stat-size bytes, no decryption. */
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = CELL_FS_O_RDONLY; ctx.gpr[5] = 0x500;
    cellFsOpen(&ctx);
    uint32_t fd = rd32(0x500);
    CHECK((uint32_t)ctx.gpr[3] == 0, "cellFsOpen of the .sdat returns CELL_OK");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x2000; ctx.gpr[5] = 0x4000; ctx.gpr[6] = 0x508;
    cellFsRead(&ctx);
    CHECK((uint32_t)ctx.gpr[3] == 0 && rd64(0x508) == (uint64_t)size,
          "cellFsRead returns all %ld container bytes (got %llu)", size, (unsigned long long)rd64(0x508));
    CHECK(memcmp(mem + 0x2000, raw, (size_t)size) == 0, "and they are the raw container, header 'NPD'");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; cellFsClose(&ctx);
    CHECK(!any_dec_beside(disc), "a plain open writes no .dec into the source tree");

    /* cellFsSdataOpen: still the plaintext. */
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = CELL_FS_O_RDONLY; ctx.gpr[5] = 0x500;
    cellFsSdataOpen(&ctx);
    fd = rd32(0x500);
    CHECK((uint32_t)ctx.gpr[3] == 0, "cellFsSdataOpen returns CELL_OK");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x2000; ctx.gpr[5] = 0x4000; ctx.gpr[6] = 0x508;
    cellFsRead(&ctx);
    CHECK((uint32_t)ctx.gpr[3] == 0 && rd64(0x508) == PLAIN && memcmp(mem + 0x2000, plain, PLAIN) == 0,
          "cellFsSdataOpen reads the %u plaintext bytes (got %llu)", PLAIN, (unsigned long long)rd64(0x508));
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; cellFsClose(&ctx);
    CHECK(!any_dec_beside(disc), "the decrypted copy is not written into the source tree");
    CHECK(any_dec_beside(cache), "it is cached under PS3RECOMP_EDAT_CACHE instead");

    printf(fails ? "FAILED: %d\n" : "OK\n", fails);
    return fails ? 1 : 0;
}

/*
 * ps3recomp - cellFs error codes, seek errors, file times and directory dots
 * (ppu_fs.cpp).
 *
 * Drives cellFsOpen / cellFsRead / cellFsWrite / cellFsLseek / cellFsFstat /
 * cellFsStat / cellFsOpendir / cellFsReaddir on real files and checks what the
 * guest sees:
 *   - the codes: an fd that is not open is CELL_EBADF (0x8001002A), a full fd
 *     table is CELL_EMFILE (0x8001002C), and CELL_FS_EIO is 0x8001002B -- it
 *     used to be 0x80010001, which is CELL_EAGAIN;
 *   - cellFsLseek: an unknown whence and a seek before the start of the file
 *     are CELL_EINVAL, the offset is left where it was, and offsets are 64-bit;
 *   - cellFsStat / cellFsFstat: the host file's atime / mtime / ctime, not 0;
 *   - cellFsReaddir: "." and ".." come back as directories.
 *
 *   tools/test_ppu_fs_errors.py [--exe-out PATH]
 *   test_fs_errors [DIR]      (DIR: scratch directory; default the system temp dir)
 */
#include "ppu_fs.cpp"
#ifdef _WIN32
#  include <windows.h>
#endif

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
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }

static int fails = 0;
#define CHECK(c, ...) do { const bool ok_ = (c); printf(ok_ ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); \
                           printf("\n"); if (!ok_) fails++; } while (0)

enum : uint32_t { PATH = 0x100, PATH2 = 0x300, FDP = 0x500, NP = 0x508, POSP = 0x510, STATB = 0x600,
                  DENT = 0x800, BUF = 0x1000 };

static uint32_t call_rc(ppu_context& ctx) { return (uint32_t)ctx.gpr[3]; }

static ppu_context mk(uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d;
    return ctx;
}
static uint32_t open_guest(const char* guest, uint32_t flags, uint32_t* fd_out)
{
    strcpy((char*)vm_base + PATH, guest);
    ppu_context ctx = mk(PATH, flags, FDP);
    cellFsOpen(&ctx);
    if (call_rc(ctx) == 0 && fd_out) *fd_out = rd32(FDP);
    return call_rc(ctx);
}
static uint32_t lseek_rc(uint32_t fd, int64_t off, uint32_t whence, uint64_t* pos)
{
    ppu_context ctx = mk(fd, (uint64_t)off, whence, POSP);
    cellFsLseek(&ctx);
    if (call_rc(ctx) == 0 && pos) *pos = rd64(POSP);
    return call_rc(ctx);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static uint8_t mem[0x20000];
    vm_base = mem; ppu_vm_size = sizeof mem;

    const char* dir = argc > 1 ? argv[1] : NULL;
#ifdef _WIN32
    char tmp[MAX_PATH];
    if (!dir) { DWORD n = GetTempPathA(sizeof tmp, tmp); if (n && n < sizeof tmp) { tmp[n - 1] = 0; dir = tmp; } }
#endif
    if (!dir) dir = getenv("TMPDIR");
    if (!dir) dir = "/tmp";
    char host[1100], hostdir[1100];
    snprintf(host, sizeof host, "%s/ps3recomp_fs_errors.bin", dir);
    snprintf(hostdir, sizeof hostdir, "%s/ps3recomp_fs_errors.d", dir);
    remove(host);
    ppu_vfs_root = dir;

    /* ---- the codes ---- */
    CHECK((uint32_t)CELL_FS_EIO == 0x8001002Bu, "CELL_FS_EIO is 0x8001002B (CELL_EIO), not EAGAIN");
    CHECK((uint32_t)CELL_FS_EBADF == 0x8001002Au, "CELL_FS_EBADF is 0x8001002A");
    CHECK((uint32_t)CELL_FS_EMFILE == 0x8001002Cu, "CELL_FS_EMFILE is 0x8001002C");

    {
        ppu_context ctx;
        ctx = mk(77, BUF, 16, NP);  cellFsRead(&ctx);
        CHECK(call_rc(ctx) == 0x8001002Au, "cellFsRead on an fd that is not open: 0x%08X (want EBADF)", call_rc(ctx));
        ctx = mk(77, BUF, 16, NP);  cellFsWrite(&ctx);
        CHECK(call_rc(ctx) == 0x8001002Au, "cellFsWrite on an fd that is not open: 0x%08X (want EBADF)", call_rc(ctx));
        ctx = mk(77, 0, 0, POSP);   cellFsLseek(&ctx);
        CHECK(call_rc(ctx) == 0x8001002Au, "cellFsLseek on an fd that is not open: 0x%08X (want EBADF)", call_rc(ctx));
        ctx = mk(77, STATB);        cellFsFstat(&ctx);
        CHECK(call_rc(ctx) == 0x8001002Au, "cellFsFstat on an fd that is not open: 0x%08X (want EBADF)", call_rc(ctx));
        ctx = mk(77, DENT, NP);     cellFsReaddir(&ctx);
        CHECK(call_rc(ctx) == 0x8001002Au, "cellFsReaddir on a dir fd that is not open: 0x%08X (want EBADF)", call_rc(ctx));
    }

    /* ---- a file to seek and stat ---- */
    uint32_t fd = 0;
    uint32_t rc = open_guest("/dev_hdd1/ps3recomp_fs_errors.bin", 0x42 /* O_RDWR|O_CREAT */, &fd);
    CHECK(rc == 0, "cellFsOpen(O_RDWR|O_CREAT) creates the file (rc 0x%08X, fd %u)", rc, fd);
    if (rc != 0) { printf("FAIL (could not create %s)\n", host); return 1; }
    memcpy(mem + BUF, "0123456789", 10);
    { ppu_context ctx = mk(fd, BUF, 10, NP); cellFsWrite(&ctx); }

    /* ---- cellFsLseek ---- */
    uint64_t pos = 99;
    CHECK(lseek_rc(fd, 4, CELL_FS_SEEK_SET, &pos) == 0 && pos == 4, "seek SET 4 -> %llu", (unsigned long long)pos);
    CHECK(lseek_rc(fd, 2, CELL_FS_SEEK_CUR, &pos) == 0 && pos == 6, "seek CUR +2 -> %llu", (unsigned long long)pos);
    CHECK(lseek_rc(fd, -3, CELL_FS_SEEK_END, &pos) == 0 && pos == 7, "seek END -3 -> %llu", (unsigned long long)pos);

    rc = lseek_rc(fd, -100, CELL_FS_SEEK_SET, NULL);
    CHECK(rc == 0x80010002u, "seek to before the start: 0x%08X (want EINVAL)", rc);
    CHECK(lseek_rc(fd, 0, CELL_FS_SEEK_CUR, &pos) == 0 && pos == 7,
          "the failed seek left the offset at 7 (now %llu)", (unsigned long long)pos);
    { ppu_context ctx = mk(fd, BUF, 3, NP); memset(mem + BUF, 0, 8); cellFsRead(&ctx);
      CHECK(call_rc(ctx) == 0 && rd64(NP) == 3 && memcmp(mem + BUF, "789", 3) == 0,
            "and a read after it returns the bytes at offset 7"); }
    rc = lseek_rc(fd, 0, 3, NULL);
    CHECK(rc == 0x80010002u, "unknown whence 3: 0x%08X (want EINVAL)", rc);
    CHECK(lseek_rc(fd, 0, CELL_FS_SEEK_CUR, &pos) == 0 && pos == 10, "offset unchanged by the bad whence");

    rc = lseek_rc(fd, 0x140000000ll, CELL_FS_SEEK_SET, &pos);
    CHECK(rc == 0 && pos == 0x140000000ull, "seek past 4 GiB keeps 64 bits (pos 0x%llX)", (unsigned long long)pos);
    lseek_rc(fd, 0, CELL_FS_SEEK_SET, NULL);

    /* ---- file times ---- */
    { ppu_context ctx = mk(fd); cellFsClose(&ctx); }
    strcpy((char*)mem + PATH, "/dev_hdd1/ps3recomp_fs_errors.bin");
    { ppu_context ctx = mk(PATH, STATB); cellFsStat(&ctx);
      const uint64_t at = rd64(STATB + 0x0C), mt = rd64(STATB + 0x14), ct = rd64(STATB + 0x1C);
      fs_stat64_t st;
      const bool have = FS_STAT64(host, &st) == 0;
      CHECK(call_rc(ctx) == 0 && have, "cellFsStat on the file");
      CHECK(mt != 0 && mt == (uint64_t)st.st_mtime, "cellFsStat mtime %llu is the host file's (%lld)",
            (unsigned long long)mt, (long long)st.st_mtime);
      CHECK(at != 0 && ct != 0, "cellFsStat atime %llu / ctime %llu are set", (unsigned long long)at, (unsigned long long)ct); }
    { uint32_t fd2 = 0;
      CHECK(open_guest("/dev_hdd1/ps3recomp_fs_errors.bin", 0, &fd2) == 0, "reopen read-only");
      ppu_context ctx = mk(fd2, STATB); cellFsFstat(&ctx);
      CHECK(call_rc(ctx) == 0 && rd64(STATB + 0x24) == 10, "cellFsFstat size %llu", (unsigned long long)rd64(STATB + 0x24));
      CHECK(rd64(STATB + 0x14) != 0 && rd64(STATB + 0x1C) != 0, "cellFsFstat mtime / ctime are set");
      ctx = mk(fd2); cellFsClose(&ctx); }

    /* ---- readdir: "." and ".." are directories ---- */
    {
#ifdef _WIN32
        _mkdir(hostdir);
#else
        mkdir(hostdir, 0755);
#endif
        char inner[1200];
        snprintf(inner, sizeof inner, "%s/inner.txt", hostdir);
        if (FILE* f = fopen(inner, "wb")) { fputs("x", f); fclose(f); }

        strcpy((char*)mem + PATH, "/dev_hdd1/ps3recomp_fs_errors.d");
        ppu_context ctx = mk(PATH, FDP); cellFsOpendir(&ctx);
        CHECK(call_rc(ctx) == 0, "cellFsOpendir");
        const uint32_t dfd = rd32(FDP);
        int dots = 0, dotdots = 0, others = 0;
        for (int i = 0; i < 16; i++) {
            memset(mem + DENT, 0, 0x102);
            ctx = mk(dfd, DENT, NP); cellFsReaddir(&ctx);
            if (call_rc(ctx) != 0 || rd64(NP) == 0) break;
            const char* name = (const char*)mem + DENT + 2;
            const uint8_t type = mem[DENT];
            if (!strcmp(name, "."))       { dots++;    CHECK(type == CELL_FS_TYPE_DIR, "'.' is typed DIR (%u)", type); }
            else if (!strcmp(name, "..")) { dotdots++; CHECK(type == CELL_FS_TYPE_DIR, "'..' is typed DIR (%u)", type); }
            else { others++; CHECK(type == CELL_FS_TYPE_REG, "'%s' is typed REG (%u)", name, type); }
        }
        CHECK(dots == 1 && dotdots == 1 && others == 1, "listing: %d '.', %d '..', %d other", dots, dotdots, others);
        ctx = mk(dfd); cellFsClosedir(&ctx);
        remove(inner);
#ifdef _WIN32
        _rmdir(hostdir);
#else
        rmdir(hostdir);
#endif
    }

    /* ---- the fd table: full is EMFILE, not EIO, and a close makes room ---- */
    {
        uint32_t fds[FS_MAX]; int n = 0;
        for (int i = 0; i < FS_MAX; i++) {
            if (open_guest("/dev_hdd1/ps3recomp_fs_errors.bin", 0, &fds[n]) != 0) break;
            n++;
        }
        CHECK(n >= 200, "opened %d descriptors before the table filled", n);
        uint32_t extra = 0;
        rc = open_guest("/dev_hdd1/ps3recomp_fs_errors.bin", 0, &extra);
        CHECK(rc == 0x8001002Cu, "open with the table full: 0x%08X (want EMFILE)", rc);
        strcpy((char*)mem + PATH, "/dev_hdd1");
        { ppu_context ctx = mk(PATH, FDP); cellFsOpendir(&ctx);
          CHECK(call_rc(ctx) == 0x8001002Cu, "opendir with the table full: 0x%08X (want EMFILE)", call_rc(ctx)); }
        { ppu_context ctx = mk(fds[0]); cellFsClose(&ctx); }
        CHECK(open_guest("/dev_hdd1/ps3recomp_fs_errors.bin", 0, &extra) == 0 && extra == fds[0],
              "closing one makes room for the next open (fd %u)", extra);
        for (int i = 1; i < n; i++) { ppu_context ctx = mk(fds[i]); cellFsClose(&ctx); }
        { ppu_context ctx = mk(extra); cellFsClose(&ctx); }
    }

    remove(host);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

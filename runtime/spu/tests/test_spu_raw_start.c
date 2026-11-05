/* Raw SPU start through the real spu_raw.c: writing 1 to SPU_RunCntl runs the
 * lifted program on a host thread of its own, and its stop is published in the
 * window the PPU polls. The host thread used to exist under _WIN32 only, so on
 * macOS and Linux a raw SPU was told to run and never did. And each start runs
 * its own program: the image found for one raw SPU used to stay pending, so
 * every later one ran the first one's program.
 *
 * The program is streamed into local store by proxy GETs (the path Tornado
 * Outbreak and Resistance use) and found by its fingerprint. The registry, the
 * channel wake-up and the lifted-code runner are stand-ins; the window, the
 * MMIO side effects and the thread are the code under test. */
#include "../spu_raw.c"

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

/* What the runtime would provide. */
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_spu_force_ch_block;
void (*g_spu_out_mbox_hook)(uint32_t, uint32_t, int, uint32_t);
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void spu_ch_wake(spu_context* c) { (void)c; }
void spu_coh_unregister(spu_context* c) { (void)c; }
void spu_mfc_release(spu_context* c) { (void)c; }
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{ (void)nid; (void)name; (void)fn; }

/* The registry: FNV-1a-64 of the exact bytes, two programs. */
uint64_t spu_workload_fingerprint(const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
static uint8_t s_prog[2][0x400];
static volatile int s_ran[2];
static pthread_t s_ran_on[2];
static void prog_a(spu_context* c) { s_ran_on[0] = pthread_self(); s_ran[0]++; c->stop_code = 0x2000; }
static void prog_b(spu_context* c) { s_ran_on[1] = pthread_self(); s_ran[1]++; c->stop_code = 0x3000; }
spu_lifted_entry_fn spu_workload_find_img(uint64_t fp, int* image_id)
{
    for (int i = 0; i < 2; i++)
        if (fp == spu_workload_fingerprint(s_prog[i], sizeof s_prog[i])) {
            if (image_id) *image_id = i + 1;
            return i ? prog_b : prog_a;
        }
    return NULL;
}
/* No program here is registered with a masked parameter area. */
spu_lifted_entry_fn spu_workload_find_img_masked(const void* data, size_t n,
                                                 uint64_t* fp_out, int* image_id)
{
    (void)data; (void)n; (void)fp_out;
    if (image_id) *image_id = 0;
    return NULL;
}
int spu_run_with_halt(void (*fn)(spu_context*), spu_context* c) { fn(c); return 0; }

static int g_fails;
#define CHECK(c, ...) do { if (!(c)) { g_fails++; fprintf(stderr, "FAIL line %d: %s: ", __LINE__, #c); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define G_PROG 0x00100000u     /* where the PPU keeps the programs it streams in */

static void w32(uint32_t ea, uint32_t v) { be32_store(ea, v); spu_raw_reg_store(ea, v, 4); }

static uint32_t create(void)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = 0x1000;
    CHECK(sc_raw_spu_create(&ctx) == CELL_OK, "create");
    return be32_load(0x1000);
}

/* Stream program `p` into raw SPU n's local store at 0, in 0x100-byte GETs. */
static void proxy_load(uint32_t n, int p)
{
    const uint32_t base = SPU_RAW_BASE + n * SPU_RAW_STRIDE;
    for (uint32_t o = 0; o < sizeof s_prog[p]; o += 0x100) {
        w32(base + SPU_RAW_MFC_LSA, o);
        w32(base + SPU_RAW_MFC_EAH, 0);
        w32(base + SPU_RAW_MFC_EAL, G_PROG + (uint32_t)p * 0x1000u + o);
        w32(base + SPU_RAW_MFC_SIZE_TAG, 0x100u << 16);
        w32(base + SPU_RAW_MFC_CLASS_CMD, 0x40u);      /* GET */
    }
}

/* Start raw SPU n and wait for the stop it publishes. */
static uint32_t run(uint32_t n)
{
    const uint32_t base = SPU_RAW_BASE + n * SPU_RAW_STRIDE;
    w32(base + SPU_RAW_NPC, 0);
    w32(base + SPU_RAW_RUNCNTL, RUNCNTL_RUN);
    uint32_t st = 0;
    for (int i = 0; i < 2000; i++) {
        if (s_spu[n].ctx && !__atomic_load_n(&s_spu[n].started, __ATOMIC_ACQUIRE)) {
            st = be32_load(base + SPU_RAW_STATUS);
            if (st & SPU_STATUS_STOPPED_BY_STOP) break;
        }
        struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL);
    }
    return st;
}

int main(void)
{
    void* p = mmap(NULL, 0x100010000ull, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    vm_base = (uint8_t*)p;
    for (int i = 0; i < 2; i++) {
        for (size_t o = 0; o < sizeof s_prog[i]; o++) s_prog[i][o] = (uint8_t)(o * (i + 3) + 1);
        if (i == 0) {   /* e_shoff 0x3D8, e_shentsize 0x28, e_shnum 1: the file is 0x400 bytes */
            s_prog[0][0x20] = 0; s_prog[0][0x21] = 0; s_prog[0][0x22] = 0x03; s_prog[0][0x23] = 0xD8;
            s_prog[0][0x2E] = 0; s_prog[0][0x2F] = 0x28;
            s_prog[0][0x30] = 0; s_prog[0][0x31] = 1;
        }
        memcpy(vm_base + G_PROG + (uint32_t)i * 0x1000u, s_prog[i], sizeof s_prog[i]);
    }

    /* 1. A proxy-loaded raw SPU runs, off the PPU's thread, and stops with its code. */
    uint32_t a = create();
    proxy_load(a, 0);
    uint32_t st = run(a);
    CHECK(s_ran[0] == 1, "program A ran %d times", s_ran[0]);
    CHECK(s_ran[0] == 0 || !pthread_equal(s_ran_on[0], pthread_self()), "ran on the caller's thread");
    CHECK(st == (SPU_STATUS_STOPPED_BY_STOP | (0x2000u << 16)), "status 0x%08X", st);
    CHECK(s_spu[a].int_stat & 0x2u, "stop-and-signal interrupt bit not set");

    /* 2. An unmatched program does not run, and SPU_RAW_DUMP_DIR writes it out
     *    as it sits in local store, named by fingerprint and LS offset. */
    {
        uint8_t other[0x400];
        for (size_t o = 0; o < sizeof other; o++) other[o] = (uint8_t)(o ^ 0x5A);
        memcpy(vm_base + G_PROG + 0x2000u, other, sizeof other);
        char dir[] = "/tmp/spu-raw-dump-XXXXXX";
        CHECK(mkdtemp(dir) != NULL, "mkdtemp");
        setenv("SPU_RAW_DUMP_DIR", dir, 1);
        uint32_t u = create();
        const uint32_t base = SPU_RAW_BASE + u * SPU_RAW_STRIDE;
        w32(base + SPU_RAW_MFC_LSA, 0);
        w32(base + SPU_RAW_MFC_EAL, G_PROG + 0x2000u);
        w32(base + SPU_RAW_MFC_SIZE_TAG, (uint32_t)sizeof other << 16);
        w32(base + SPU_RAW_MFC_CLASS_CMD, 0x40u);
        w32(base + SPU_RAW_RUNCNTL, RUNCNTL_RUN);
        CHECK(s_spu[u].ctx == NULL && !s_spu[u].started, "an unmatched program started");
        char path[256];
        snprintf(path, sizeof path, "%s/rawspu_%016llX_00000.bin", dir,
                 (unsigned long long)spu_workload_fingerprint(other, sizeof other));
        FILE* f = fopen(path, "rb");
        CHECK(f != NULL, "no dump at %s", path);
        if (f) {
            uint8_t got[0x500];
            size_t n = fread(got, 1, sizeof got, f);
            fclose(f);
            CHECK(n == sizeof other && memcmp(got, other, n) == 0, "dump is %zu bytes", n);
            remove(path);
        }
        rmdir(dir);
        unsetenv("SPU_RAW_DUMP_DIR");
    }

    /* 3. An imported image runs on the start after the import, and only that
     *    one: the next raw SPU streams its own program in and runs it, rather
     *    than the image imported for the one before. */
    {
        uint8_t* e = vm_base + G_PROG;          /* program A, as an "ELF" of 0x400 bytes */
        int before_a = s_ran[0], before_b = s_ran[1];
        spu_raw_note_image(G_PROG, 0);
        CHECK(s_pending_entry == prog_a, "import of A not found (shoff %02X%02X)", e[0x22], e[0x23]);
        uint32_t i1 = create();
        st = run(i1);
        CHECK(s_ran[0] == before_a + 1, "imported A ran %d times", s_ran[0] - before_a);
        uint32_t i2 = create();
        proxy_load(i2, 1);
        st = run(i2);
        CHECK(s_ran[1] == before_b + 1 && s_ran[0] == before_a + 1,
              "second SPU ran A %d, B %d times", s_ran[0] - before_a, s_ran[1] - before_b);
        CHECK(st == (SPU_STATUS_STOPPED_BY_STOP | (0x3000u << 16)), "status 0x%08X", st);

        /* 4. Restarted with nothing newly loaded (the PPU serviced its stop):
         *    the program it already had. */
        st = run(i2);
        CHECK(s_ran[1] == before_b + 2 && s_ran[0] == before_a + 1,
              "restart ran A %d, B %d times", s_ran[0] - before_a, s_ran[1] - before_b);
        free(s_spu[i1].ctx);
        free(s_spu[i2].ctx);
    }

    free(s_spu[a].ctx);
    if (g_fails) { fprintf(stderr, "%d check(s) failed\n", g_fails); return 1; }
    puts("PASS: a raw SPU started by SPU_RunCntl runs on its own host thread and publishes its stop");
    return 0;
}

/*
 * test_savedata_param_sfo - cellSaveData's PARAM.SFO is a binary PSF.
 *
 * A save's PARAM.SFO has to be a PSF 1.1 file (layout: the PS3 developer wiki's
 * "PARAM.SFO" page) or RPCS3 and a PS3 reject the save. The HLE used to write a
 * ~100-byte "TITLE=..." text file. This test
 *   - writes a save's PARAM.SFO and checks it with a PSF reader that shares no
 *     code with the writer: magic and version, table offsets, alphabetical keys,
 *     4-byte alignment, formats, len/max_len and every value;
 *   - reads it back through read_param_sfo;
 *   - reads an old text-format file (existing saves must keep loading) and
 *     checks that the next write turns it into a PSF;
 *   - feeds a PSF whose last key has no terminating NUL inside the buffer to
 *     the parser, with the buffer ending at a PROT_NONE page (POSIX only).
 *
 *   cc -std=gnu17 -I include -o test_savedata_param_sfo libs/system/tests/test_savedata_param_sfo.c
 */
#include "../cellSaveData.c"

#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_linelock_lock(uint32_t a) { (void)a; }
void spu_linelock_release(uint32_t a) { (void)a; }

ps3_guest_caller_fn g_ps3_guest_caller;
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*)) { (void)nid; (void)name; (void)fn; }

/* ---- an independent PSF reader (little-endian fields read byte by byte) ---- */
static uint32_t le16(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t* p) { return le16(p) | le16(p + 2) << 16; }

typedef struct { const char* key; uint32_t fmt, len, max_len; const uint8_t* data; } entry_t;

static const char* const WANT_KEYS[] = {
    "ACCOUNT_ID", "ATTRIBUTE", "CATEGORY", "DETAIL", "PARAMS", "PARAMS2", "PARENTAL_LEVEL",
    "SAVEDATA_DIRECTORY", "SAVEDATA_LIST_PARAM", "SUB_TITLE", "TITLE" };
#define N_KEYS (sizeof WANT_KEYS / sizeof WANT_KEYS[0])

/* Validates the structure and fills e[] (N_KEYS entries). */
static void read_psf(const uint8_t* b, size_t size, entry_t* e)
{
    assert(size >= 0x14);
    assert(b[0] == 0 && b[1] == 'P' && b[2] == 'S' && b[3] == 'F');
    assert(b[4] == 1 && b[5] == 1 && b[6] == 0 && b[7] == 0);        /* version 1.1 */
    uint32_t kt = le32(b + 8), dt = le32(b + 12), n = le32(b + 16);
    assert(n == N_KEYS);
    assert(kt == 0x14 + 16 * n);                                      /* key table right after the index */
    assert(dt % 4 == 0 && dt > kt);
    const char* prev = "";
    uint32_t total = dt;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* x = b + 0x14 + 16 * i;
        e[i].key = (const char*)b + kt + le16(x);
        e[i].fmt = le16(x + 2);
        e[i].len = le32(x + 4);
        e[i].max_len = le32(x + 8);
        e[i].data = b + dt + le32(x + 12);
        assert(kt + le16(x) < dt);
        assert(i == 0 || strcmp(prev, e[i].key) < 0);                 /* alphabetical */
        assert(strcmp(e[i].key, WANT_KEYS[i]) == 0);
        assert(e[i].max_len % 4 == 0 && e[i].len <= e[i].max_len);
        assert(le32(x + 12) + e[i].max_len <= size - dt);
        total += e[i].max_len;
        prev = e[i].key;
    }
    assert(total == size);
}

static void check_text(const entry_t* e, const char* key, uint32_t max_len, const char* value)
{
    assert(strcmp(e->key, key) == 0);
    assert(e->fmt == 0x0204 && e->max_len == max_len);                /* utf8, NUL counted in len */
    assert(e->len == strlen(value) + 1 && e->data[e->len - 1] == 0);
    assert(memcmp(e->data, value, e->len) == 0);
}

static void check_int(const entry_t* e, const char* key, uint32_t value)
{
    assert(strcmp(e->key, key) == 0);
    assert(e->fmt == 0x0404 && e->len == 4 && e->max_len == 4);
    assert(le32(e->data) == value);
}

static void check_raw(const entry_t* e, const char* key, uint32_t size)
{
    assert(strcmp(e->key, key) == 0);
    assert(e->fmt == 0x0004 && e->len == size && e->max_len == size);
}

static size_t slurp(const char* path, uint8_t* buf, size_t cap)
{
    FILE* f = fopen(path, "rb");
    assert(f);
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

int main(void)
{
    char root[] = "/tmp/ps3recomp-savedata-sfo-XXXXXX";
    assert(mkdtemp(root));
    char dir[256], sfo[300];
    snprintf(dir, sizeof dir, "%s/GAME0", root);
    assert(mkdir(dir, 0755) == 0);
    snprintf(sfo, sizeof sfo, "%s/PARAM.SFO", dir);

    CellSaveDataSystemFileParam p;
    memset(&p, 0, sizeof p);
    strcpy(p.title, "Test Game");
    strcpy(p.subTitle, "Chapter 2 \xC3\xA9");
    strcpy(p.detail, "Checkpoint 7\nline two");
    p.attribute = 0x20;
    strcpy(p.listParam, "CONFIG");
    write_param_sfo(dir, &p);

    /* the file the writer produced is a PSF with the game-save field set */
    static uint8_t raw[8192];
    size_t size = slurp(sfo, raw, sizeof raw);
    entry_t e[N_KEYS];
    read_psf(raw, size, e);
    check_raw(&e[0], "ACCOUNT_ID", 16);
    check_int(&e[1], "ATTRIBUTE", 0x20);
    check_text(&e[2], "CATEGORY", 4, "SD");
    check_text(&e[3], "DETAIL", 1024, "Checkpoint 7\nline two");
    check_raw(&e[4], "PARAMS", 1024);
    check_raw(&e[5], "PARAMS2", 12);
    check_int(&e[6], "PARENTAL_LEVEL", 0);
    check_text(&e[7], "SAVEDATA_DIRECTORY", 64, "GAME0");
    check_text(&e[8], "SAVEDATA_LIST_PARAM", 8, "CONFIG");
    check_text(&e[9], "SUB_TITLE", 128, "Chapter 2 \xC3\xA9");
    check_text(&e[10], "TITLE", 128, "Test Game");

    /* and read_param_sfo returns what was written */
    CellSaveDataSystemFileParam q;
    memset(&q, 0, sizeof q);
    read_param_sfo(dir, &q);
    assert(strcmp(q.title, p.title) == 0 && strcmp(q.subTitle, p.subTitle) == 0);
    assert(strcmp(q.detail, p.detail) == 0 && strcmp(q.listParam, p.listParam) == 0);
    assert(q.attribute == 0x20);

    /* text longer than the field is cut to max_len - 1 and keeps its NUL */
    memset(p.title, 'T', 127); p.title[127] = 0;
    memset(p.listParam, 'L', 7); p.listParam[7] = 0;
    write_param_sfo(dir, &p);
    size = slurp(sfo, raw, sizeof raw);
    read_psf(raw, size, e);
    assert(e[10].len == 128 && e[8].len == 8);

    /* a save written by an earlier build is the old text file: it still loads... */
    FILE* f = fopen(sfo, "wb");
    assert(f);
    fputs("TITLE=Old T\nSUB_TITLE=Old S\nDETAIL=Old D\nATTRIBUTE=5\nLIST_PARAM=LP\n", f);
    fclose(f);
    memset(&q, 0, sizeof q);
    read_param_sfo(dir, &q);
    assert(strcmp(q.title, "Old T") == 0 && strcmp(q.subTitle, "Old S") == 0);
    assert(strcmp(q.detail, "Old D") == 0 && strcmp(q.listParam, "LP") == 0 && q.attribute == 5);
    /* ...and the next save rewrites it as a PSF */
    write_param_sfo(dir, &q);
    size = slurp(sfo, raw, sizeof raw);
    read_psf(raw, size, e);
    check_text(&e[10], "TITLE", 128, "Old T");

    /* a corrupt PSF must not make the parser run off its buffer: move the key table to the last three bytes of the
     * file, so the first key ("TIT") has no NUL inside the buffer, and place the buffer against a PROT_NONE page */
#ifdef PS3_PSF_H
    {
        size_t n = size;
        raw[8] = (uint8_t)(n - 3); raw[9] = (uint8_t)((n - 3) >> 8); raw[10] = raw[11] = 0;
        raw[n - 3] = 'T'; raw[n - 2] = 'I'; raw[n - 1] = 'T';
        size_t pg = (size_t)sysconf(_SC_PAGESIZE), span = (n + pg - 1) / pg * pg;
        uint8_t* m = mmap(0, span + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(m != MAP_FAILED);
        assert(mprotect(m + span, pg, PROT_NONE) == 0);
        uint8_t* cut = m + span - n;
        memcpy(cut, raw, n);
        char t[128] = "", s[128] = "", d[1024] = "", l[8] = "";
        uint32_t attr = 0;
        assert(ps3_psf_parse(cut, n, t, sizeof t, s, sizeof s, d, sizeof d, &attr, l, sizeof l) == 1);
        munmap(m, span + pg);
    }
#endif

    snprintf(sfo, sizeof sfo, "%s/PARAM.SFO", dir);
    assert(unlink(sfo) == 0);
    assert(rmdir(dir) == 0);
    assert(rmdir(root) == 0);
    printf("test_savedata_param_sfo: all passed\n");
    return 0;
}

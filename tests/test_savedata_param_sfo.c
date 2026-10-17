/* Save PARAM.SFO: a real PSF is written (the format a PS3 and RPCS3 read), a
 * console's account binding survives a re-save, and both PSF and the KEY=value
 * text this runtime used to write are read back. CPU only; no game data.
 * Usage: test_savedata_param_sfo <empty scratch directory> */
#include "../libs/system/cellSaveData.c"
#include <assert.h>
uint8_t* vm_base;

static unsigned checks;
#define CHECK(v) do { ++checks; if (!(v)) { fprintf(stderr, "FAIL line%d: %s\n", __LINE__, #v); return 1; } } while (0)

static int make_dir(const char* path)
{
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

static size_t read_all(const char* dir, u8* out, size_t cap)
{
    char path[1024];
    sfo_path_of(path, sizeof path, dir);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(out, 1, cap, f);
    fclose(f);
    return n;
}

static void write_all(const char* dir, const void* data, size_t n)
{
    char path[1024];
    sfo_path_of(path, sizeof path, dir);
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(data, 1, n, f); fclose(f); }
}

static CellSaveDataSystemFileParam sample(const char* title)
{
    CellSaveDataSystemFileParam p;
    memset(&p, 0, sizeof p);
    snprintf(p.title, sizeof p.title, "%s", title);
    snprintf(p.subTitle, sizeof p.subTitle, "Keats Faery Realm Elysium");
    snprintf(p.detail, sizeof p.detail, "Keats Prologue\nEllen Prologue\nPlay time 00:55");
    p.attribute = 1;
    p.reserved2[3] = 3;                     /* parental level 3, big-endian */
    memcpy(p.listParam, "FIELD", 5);
    return p;
}

int main(int argc, char** argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s <scratch dir>\n", argv[0]); return 2; }
    char dir[1024], legacy[1024], broken[1024];
    snprintf(dir, sizeof dir, "%s/BCUS98147-SAVEDATA-0000", argv[1]);
    snprintf(legacy, sizeof legacy, "%s/LEGACY", argv[1]);
    snprintf(broken, sizeof broken, "%s/BROKEN", argv[1]);
    CHECK(make_dir(dir) && make_dir(legacy) && make_dir(broken));

    /* Written as a real PSF with the save keys, sorted. */
    const CellSaveDataSystemFileParam in = sample("Folklore\xE2\x84\xA2");
    write_param_sfo(dir, "BCUS98147-SAVEDATA-0000", &in);
    static u8 d[SFO_MAX_BYTES];
    size_t n = read_all(dir, d, sizeof d);
    CHECK(n > 20 && !memcmp(d, "\0PSF", 4) && sfo_get32(d + 4) == 0x101);
    const u32 count = sfo_get32(d + 16), keys = sfo_get32(d + 8);
    CHECK(count == 11);
    for (u32 i = 1; i < count; i++)
        CHECK(strcmp((const char*)d + keys + sfo_get16(d + 20 + 16 * (i - 1)),
                     (const char*)d + keys + sfo_get16(d + 20 + 16 * i)) < 0);
    char text[1100] = {0};
    sfo_copy_string(d, n, "CATEGORY", text, sizeof text);
    CHECK(!strcmp(text, "SD"));
    sfo_copy_string(d, n, "SAVEDATA_DIRECTORY", text, sizeof text);
    CHECK(!strcmp(text, "BCUS98147-SAVEDATA-0000"));
    sfo_copy_string(d, n, "DETAIL", text, sizeof text);
    CHECK(!strcmp(text, in.detail));
    u32 format = 0, length = 0;
    CHECK(sfo_find(d, n, "PARAMS", &format, &length) && format == SFO_BYTES && length == 1024);
    const u8* account = sfo_find(d, n, "ACCOUNT_ID", &format, &length);
    CHECK(account && length == 16 && !memcmp(account, "0000000000000000", 16));

    /* Read back field for field. */
    CellSaveDataSystemFileParam out;
    read_param_sfo(dir, &out);
    CHECK(!strcmp(out.title, in.title) && !strcmp(out.subTitle, in.subTitle));
    CHECK(!strcmp(out.detail, in.detail) && !memcmp(out.listParam, "FIELD", 6));
    CHECK(out.attribute == 1 && out.reserved2[3] == 3 && !out.reserved2[0]);

    /* A console's binding survives the game saving over it here. */
    u8* acc = (u8*)sfo_find(d, n, "ACCOUNT_ID", &format, &length);
    memcpy(acc, "1234567890ABCDEF", 16);
    u8* params = (u8*)sfo_find(d, n, "PARAMS", &format, &length);
    params[0] = 0xAA; params[1023] = 0x55;
    write_all(dir, d, n);
    const CellSaveDataSystemFileParam again = sample("Folklore again");
    write_param_sfo(dir, "BCUS98147-SAVEDATA-0000", &again);
    n = read_all(dir, d, sizeof d);
    account = sfo_find(d, n, "ACCOUNT_ID", &format, &length);
    CHECK(account && !memcmp(account, "1234567890ABCDEF", 16));
    const u8* kept = sfo_find(d, n, "PARAMS", &format, &length);
    CHECK(kept && kept[0] == 0xAA && kept[1023] == 0x55);
    read_param_sfo(dir, &out);
    CHECK(!strcmp(out.title, "Folklore again"));

    /* The KEY=value text this runtime used to write still loads. */
    static const char old[] = "TITLE=Folklore\nSUB_TITLE=Keats Faery Realm Elysium\n"
                              "DETAIL=Keats Prologue\nATTRIBUTE=0\nLIST_PARAM=FIELD\n";
    write_all(legacy, old, sizeof old - 1);
    read_param_sfo(legacy, &out);
    CHECK(!strcmp(out.title, "Folklore") && !strcmp(out.subTitle, "Keats Faery Realm Elysium"));
    CHECK(!memcmp(out.listParam, "FIELD", 6));

    /* A truncated PSF reads as empty rather than out of bounds. */
    static const u8 cut[24] = { 0, 'P', 'S', 'F', 1, 1, 0, 0, 0x40, 0, 0, 0, 0x80, 0, 0, 0, 5, 0, 0, 0 };
    write_all(broken, cut, sizeof cut);
    read_param_sfo(broken, &out);
    CHECK(!out.title[0] && !out.subTitle[0] && !out.attribute);

    printf("save PARAM.SFO: %u checks passed\n", checks);
    return 0;
}

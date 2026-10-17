/* cellGameBootCheck reports the game type from the PARAM.SFO CATEGORY: "HG" (a PSN download that boots from
 * /dev_hdd0/game/<id>) is an HDD game, "DG" a disc game, and a title without a CATEGORY keeps the default (disc).
 * A title that checks the type and does not get the one it expects gives up at boot.
 *
 * clang -std=gnu17 -I include libs/system/tests/test_game_category.c
 * -Wl,-dead_strip -o /tmp/test_game_category && /tmp/test_game_category */
#include "../cellGame.c"
#include <assert.h>
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

ps3_guest_caller_fn g_ps3_guest_caller;
const char* ppu_vfs_root;   /* the loader's; cellGame_set_content_path overrides it */

static void put16(unsigned char* p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char* p, unsigned v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

/* A PARAM.SFO of the given string entries (key, value), in the layout sfo_read_string reads. */
static void write_sfo(const char* path, const char* const (*kv)[2], int n)
{
    unsigned char hdr[0x14 + 8 * 0x10] = { 0 };
    unsigned char keys[256], data[256];
    unsigned ko = 0, dof = 0;
    memcpy(hdr, "\0PSF", 4);
    put32(hdr + 4, 0x101);
    put32(hdr + 0x10, (unsigned)n);
    for (int i = 0; i < n; i++) {
        unsigned kl = (unsigned)strlen(kv[i][0]) + 1, vl = (unsigned)strlen(kv[i][1]) + 1;
        unsigned char* e = hdr + 0x14 + i * 0x10;
        put16(e + 0, ko); put16(e + 2, 0x0204); put32(e + 4, vl); put32(e + 8, vl); put32(e + 12, dof);
        memcpy(keys + ko, kv[i][0], kl); ko += kl;
        memcpy(data + dof, kv[i][1], vl); dof += vl;
    }
    const unsigned key_tab = 0x14 + (unsigned)n * 0x10;
    put32(hdr + 8, key_tab);
    put32(hdr + 0xC, key_tab + ko);
    FILE* f = fopen(path, "wb");
    assert(f);
    fwrite(hdr, 1, key_tab, f); fwrite(keys, 1, ko, f); fwrite(data, 1, dof, f);
    fclose(f);
}

static unsigned boot_type(const char* sfo_path, const char* category)
{
    if (category) {
        const char* const kv[2][2] = { { "TITLE_ID", "TEST00000" }, { "CATEGORY", category } };
        write_sfo(sfo_path, kv, 2);
    } else {
        const char* const kv[1][2] = { { "TITLE_ID", "TEST00000" } };
        write_sfo(sfo_path, kv, 1);
    }
    cellGame_init_from_paramsfo(sfo_path);
    assert(cellGameBootCheck((u32*)0x100, (u32*)0x104, (CellGameContentSize*)0x200, (char*)0x300) == CELL_OK);
    return vm_read32(0x100);
}

int main(void)
{
    char root[] = "/tmp/ps3recomp-game-category-XXXXXX";
    assert(mkdtemp(root));
    char sfo[256];
    snprintf(sfo, sizeof sfo, "%s/PARAM.SFO", root);
    vm_base = calloc(1, 65536); assert(vm_base);
    cellGame_set_content_path(root);

    assert(boot_type(sfo, NULL) == CELL_GAME_GAMETYPE_DISC);      /* no CATEGORY: the default */
    assert(boot_type(sfo, "HG") == CELL_GAME_GAMETYPE_HDD);
    assert(boot_type(sfo, "DG") == CELL_GAME_GAMETYPE_DISC);
    assert(boot_type(sfo, "HG") == CELL_GAME_GAMETYPE_HDD);
    assert(boot_type(sfo, "XX") == CELL_GAME_GAMETYPE_HDD);       /* an unmapped CATEGORY changes nothing */

    assert(unlink(sfo) == 0); assert(rmdir(root) == 0);
    free(vm_base);
    puts("cellGameBootCheck reports the game type from PARAM.SFO CATEGORY");
}

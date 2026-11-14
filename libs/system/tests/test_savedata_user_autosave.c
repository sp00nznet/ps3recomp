/*
 * cellSaveDataUserAutoSave / UserAutoLoad take nine arguments; the ninth
 * (userdata) is in the caller's parameter save area at SP+112. Both need a
 * context handler: through the generic eight-argument binding the title's
 * callbacks see cbResult->userdata == 0.
 *
 * cellSaveDataUserListAutoLoad / UserListAutoSave take ten: userId follows
 * version, and container and userdata are the ninth and tenth, at SP+112 and
 * SP+120. Their funcFixed picks the directory, funcStat then runs on it.
 *
 * This registers cellSaveData's context handlers into a local table, then
 * calls each handler the way the dispatcher does and checks the userdata
 * the callbacks receive. Built and run by
 * tools/test_savedata_user_autosave.py, which passes a scratch directory (the
 * save root) as argv[1] and removes it afterwards.
 */
#include "../cellSaveData.c"
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

static struct { uint32_t nid; void (*fn)(ppu_context*); } s_reg[16];
static int s_nreg;
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    (void)name;
    if (s_nreg < 16) { s_reg[s_nreg].nid = nid; s_reg[s_nreg].fn = fn; s_nreg++; }
}
static void (*find(uint32_t nid))(ppu_context*)
{
    for (int i = 0; i < s_nreg; i++) if (s_reg[i].nid == nid) return s_reg[i].fn;
    return NULL;
}

#define FIXED_OPD 0x800u
#define STAT_OPD 0x900u
#define PARAM_EA 0x1000u     /* a CellSaveDataSystemFileParam the stat callback hands back */
#define LISTED_DIR "TEST00001"
static int s_stat_calls, s_fixed_calls;
static uint32_t s_seen_userdata, s_fixed_userdata, s_fixed_dir_num;
static int s_stat_sets_param;
static void guest_callback(u32 opd, u64 cb, u64 get, u64 set,
                           u64 a, u64 b, u64 c, u64 d, u64 e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    if (opd == FIXED_OPD) {
        s_fixed_calls++;
        s_fixed_userdata = vm_read32((u32)cb + 16);
        s_fixed_dir_num = vm_read32((u32)get);
        vm_write32((u32)set, vm_read32((u32)get + 8));  /* dirName: the first entry of the list */
        vm_write32((u32)set + 8, 0);                    /* option */
        return;                                         /* result stays OK_NEXT */
    }
    if (opd != STAT_OPD) { vm_write32((u32)cb, (u32)-1); return; }
    s_stat_calls++;
    s_seen_userdata = vm_read32((u32)cb + 16);
    if (s_stat_sets_param) vm_write32((u32)set, PARAM_EA);
    vm_write32((u32)cb, 1);  /* OK_LAST: finish without a file operation */
}

static int s_failed;
static void check(int ok, const char* what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) s_failed++;
}

/* Call a nine-argument savedata handler as the dispatcher would. */
static void run(const char* name, uint32_t nid, uint32_t userdata)
{
    char what[160];
    void (*fn)(ppu_context*) = find(nid);
    snprintf(what, sizeof what, "%s (NID 0x%08X) has a context handler", name, nid);
    check(fn != NULL, what);
    if (!fn) return;
    ppu_context ctx = {0};
    ctx.gpr[1]  = 0x400;          /* caller SP */
    ctx.gpr[3]  = 0;              /* version */
    ctx.gpr[4]  = 1;              /* userId */
    ctx.gpr[5]  = 0x300;          /* dirName */
    ctx.gpr[6]  = 0;              /* errDialog */
    ctx.gpr[7]  = 0x200;          /* setBuf */
    ctx.gpr[8]  = STAT_OPD;       /* funcStat */
    ctx.gpr[9]  = 0xA00;          /* funcFile */
    ctx.gpr[10] = 0;              /* container */
    vm_write64(0x400 + 112, userdata);
    s_stat_calls = 0; s_seen_userdata = 0;
    fn(&ctx);
    snprintf(what, sizeof what, "%s: funcStat ran once", name);
    check(s_stat_calls == 1, what);
    snprintf(what, sizeof what, "%s: cbResult->userdata = 0x%08X (want 0x%08X)",
             name, s_seen_userdata, userdata);
    check(s_seen_userdata == userdata, what);
}

static int param_sfo_exists(void)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/" LISTED_DIR "/PARAM.SFO", s_save_root);
    FILE* f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* Call a ten-argument savedata handler as the dispatcher does. container is
 * the ninth argument and userdata the tenth: both on the caller's stack. */
static void run10(const char* name, uint32_t nid, int is_save, uint32_t userdata)
{
    char what[160];
    void (*fn)(ppu_context*) = find(nid);
    snprintf(what, sizeof what, "%s (NID 0x%08X) has a context handler", name, nid);
    check(fn != NULL, what);
    if (!fn) return;
    ppu_context ctx = {0};
    ctx.gpr[1]  = 0x400;          /* caller SP */
    ctx.gpr[3]  = 0;              /* version */
    ctx.gpr[4]  = 1;              /* userId */
    ctx.gpr[5]  = 0;              /* errDialog */
    ctx.gpr[6]  = 0x100;          /* setList: prefix at +8 */
    ctx.gpr[7]  = 0x200;          /* setBuf: dirListMax at +0 */
    ctx.gpr[8]  = FIXED_OPD;      /* funcFixed */
    ctx.gpr[9]  = STAT_OPD;       /* funcStat */
    ctx.gpr[10] = 0xA00;          /* funcFile */
    vm_write64(0x400 + 112, 0x1234);    /* container: must not become userdata */
    vm_write64(0x400 + 120, userdata);
    s_fixed_calls = s_stat_calls = 0;
    s_fixed_userdata = s_seen_userdata = s_fixed_dir_num = 0;
    s_stat_sets_param = is_save;
    fn(&ctx);
    snprintf(what, sizeof what, "%s: returns CELL_OK", name);
    check((s32)ctx.gpr[3] == CELL_OK, what);
    snprintf(what, sizeof what, "%s: funcFixed ran once and listed the one matching directory", name);
    check(s_fixed_calls == 1 && s_fixed_dir_num == 1, what);
    snprintf(what, sizeof what, "%s: funcStat ran once on the chosen directory", name);
    check(s_stat_calls == 1, what);
    snprintf(what, sizeof what, "%s: userdata 0x%08X reached funcFixed (0x%08X) and funcStat (0x%08X)",
             name, userdata, s_fixed_userdata, s_seen_userdata);
    check(s_fixed_userdata == userdata && s_seen_userdata == userdata, what);
    snprintf(what, sizeof what, "%s: PARAM.SFO %s", name,
             is_save ? "is written (the save direction)" : "is not written (the load direction)");
    check(param_sfo_exists() == is_save, what);
}

int main(int argc, char** argv)
{
    if (argc < 2 || strlen(argv[1]) >= sizeof s_save_root) {
        puts("FAIL: usage: test_savedata_user_autosave SCRATCH_DIR");
        return 1;
    }
    strcpy(s_save_root, argv[1]);
    vm_base = calloc(1, 0x200000);
    if (!vm_base) { puts("FAIL: calloc"); return 1; }
    if (cellSaveData_set_scratch_region(0x100000, 0x20000) != CELL_OK) {
        puts("FAIL: scratch region"); return 1;
    }
    strcpy((char*)vm_base + 0x300, "TEST00001");
    g_ps3_guest_caller = guest_callback;

    cellSaveData_register_ctx_handlers();
    run("cellSaveDataUserAutoLoad", 0xCDC6AEFDu, 0x0001CAFEu);
    run("cellSaveDataUserAutoSave", 0x52AAC4FAu, 0x0001BEEFu);

    /* The list variants: one save directory under the prefix "TEST". A title
     * with no PARAM.SFO of its own (an empty directory) is a new save. */
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/" LISTED_DIR, s_save_root);
    if (mkdir(dir, 0700) != 0) { puts("FAIL: mkdir"); return 1; }
    strcpy((char*)vm_base + 0x340, "TEST");
    vm_write32(0x108, 0x340);     /* setList.prefix */
    vm_write32(0x200, 1);         /* setBuf.dirListMax */
    memset(vm_base + PARAM_EA, 0, sizeof(CellSaveDataSystemFileParam));
    strcpy((char*)vm_base + PARAM_EA, "Test save");
    run10("cellSaveDataUserListAutoLoad", 0x248BD1D8u, 0, 0x0002CAFEu);
    run10("cellSaveDataUserListAutoSave", 0x0E091C36u, 1, 0x0002BEEFu);

    free(vm_base);
    if (s_failed) { printf("FAIL (%d failed)\n", s_failed); return 1; }
    puts("PASS (0 failed)");
    return 0;
}

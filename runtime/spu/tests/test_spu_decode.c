/* The interpreter's decode contract: op and every field spu_decode1 hands to
 * the execute switch, for instructions at the opcode-width and immediate-form
 * boundaries. Encodings are built here from the SPU ISA field layouts (IBM SPU
 * ISA v1.2, ch. 2), not from the generated table, so a table whose precedence
 * or immediate forms drift fails against independent expectations. spu_decode1
 * is static, hence the include of the interpreter itself. */
#include "../spu_interp.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The decode checks execute nothing: the external hooks must never run. */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c; (void)a; abort(); }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; abort(); }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr; abort();
}

/* ISA layouts, bit 0 = LSB. */
static uint32_t rrr(uint32_t op4, uint32_t rt, uint32_t rb, uint32_t ra, uint32_t rc) {
    return (op4 << 28) | (rt << 21) | (rb << 14) | (ra << 7) | rc;
}
static uint32_t ri18(uint32_t op7, uint32_t i18, uint32_t rt) {
    return (op7 << 25) | ((i18 & 0x3FFFF) << 7) | rt;
}
static uint32_t ri16(uint32_t op9, uint32_t i16, uint32_t rt) {
    return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt;
}
static uint32_t ri10(uint32_t op8, uint32_t i10, uint32_t ra, uint32_t rt) {
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt;
}
static uint32_t ri8(uint32_t op10, uint32_t i8, uint32_t ra, uint32_t rt) {
    return (op10 << 22) | ((i8 & 0xFF) << 14) | (ra << 7) | rt;
}
/* RI7 and RR share the 11-bit opcode; RR's rb sits where RI7's i7 does. */
static uint32_t ri7(uint32_t op11, uint32_t i7, uint32_t ra, uint32_t rt) {
    return (op11 << 21) | ((i7 & 0x7F) << 14) | (ra << 7) | rt;
}

static int failures;
static spu_ins dec(uint32_t insn, uint32_t pc) {
    spu_ins d;
    memset(&d, 0xFF, sizeof d); /* decode must overwrite every field, not leak the caller's */
    spu_decode1(insn, pc, &d);
    return d;
}
#define CHECK(cond, what, insn) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s insn=%08X: %s\n", what, (unsigned)(insn), #cond); ++failures; } } while (0)

static void expect_imm(const char* what, uint32_t insn, spu_op op, int32_t imm) {
    spu_ins d = dec(insn, 0x100);
    CHECK(d.op == op, what, insn);
    CHECK(d.imm == imm, what, insn);
    CHECK(d.tgt == 0, what, insn);
}
static void expect_tgt(const char* what, uint32_t insn, uint32_t pc, spu_op op, uint32_t tgt) {
    spu_ins d = dec(insn, pc);
    CHECK(d.op == op, what, insn);
    CHECK(d.tgt == tgt, what, insn);
    CHECK(d.imm == 0, what, insn);
}

/* The decode above is what makes the interpreter stop: run `il r2,5 / <word> / il r3,1 /
 * stop 0` and check the SPU halts at the word's own pc with the following instructions
 * unexecuted. Only CPU instructions run, so the external hooks above still never fire. */
static spu_context run_ctx;
static void put_word(uint32_t at, uint32_t w) {
    run_ctx.ls[at] = (uint8_t)(w >> 24); run_ctx.ls[at + 1] = (uint8_t)(w >> 16);
    run_ctx.ls[at + 2] = (uint8_t)(w >> 8); run_ctx.ls[at + 3] = (uint8_t)w;
}
static void check_stops_at_word(uint32_t op11) {
    const uint32_t word = (op11 << 21) | (0x33u << 14) | (0x22u << 7) | 0x11u;
    memset(&run_ctx, 0, sizeof run_ctx);
    spu_context_init(&run_ctx, 0);
    run_ctx.image_id = -1;                      /* interpret, no lifted-function lookup */
    put_word(0x100, ri16(0x081, 5, 2));         /* il r2,5 */
    put_word(0x104, word);
    put_word(0x108, ri16(0x081, 1, 3));         /* il r3,1: must not run */
    put_word(0x10C, 0);                         /* stop 0 */
    (void)spu_interp_run(&run_ctx, 0x100);
    CHECK(run_ctx.status == SPU_STATUS_STOPPED_BY_HALT && run_ctx.pc == 0x104, "halt at the word", word);
    CHECK(run_ctx.gpr[2]._u32[0] == 5 && run_ctx.gpr[3]._u32[0] == 0, "nothing after the word ran", word);
}

int main(void) {
    /* RRR: shufb rt=5 rb=6 ra=7 rc=8. The destination is the high field, which
     * the interpreter calls rc; the low field is the third source (rt). */
    {
        uint32_t i = rrr(0xB, 5, 6, 7, 8);
        spu_ins d = dec(i, 0);
        CHECK(d.op == SPU_shufb, "shufb", i);
        CHECK(d.rc == 5 && d.rb == 6 && d.ra == 7 && d.rt == 8, "shufb fields", i);
        CHECK(d.imm == 0 && d.tgt == 0, "shufb no imm", i);
    }

    /* RI18 */
    expect_imm("ila 18 bits", ri18(0x21, 0x3FFFF, 3), SPU_ila, 0x3FFFF);
    expect_imm("ila high bit", ri18(0x21, 0x20000, 3), SPU_ila, 0x20000);
    {
        spu_ins d = dec(ri18(0x21, 0x2ABCD, 9), 0);
        CHECK(d.rt == 9, "ila rt", 0);
    }

    /* Hints (hbra/hbrr) are the other RI18 ops: only the low 16 bits of the
     * 18-bit field are the immediate; the top two belong to the split ROH field. */
    expect_imm("hbra low 16", ri18(0x08, 0x3FFFF, 3), SPU_hbra, 0xFFFF);
    expect_imm("hbrr low 16", ri18(0x09, 0x2ABCD, 3), SPU_hbrr, 0xABCD);

    /* RI16 immediates: il sign-extends; ilh/ilhu/iohl/fsmbi zero-extend. */
    expect_imm("il negative", ri16(0x081, 0x8000, 4), SPU_il, -32768);
    expect_imm("il -1", ri16(0x081, 0xFFFF, 4), SPU_il, -1);
    expect_imm("il positive", ri16(0x081, 0x7FFF, 4), SPU_il, 0x7FFF);
    expect_imm("ilhu", ri16(0x082, 0xFFFF, 4), SPU_ilhu, 0xFFFF);
    expect_imm("ilh", ri16(0x083, 0x8001, 4), SPU_ilh, 0x8001);
    expect_imm("iohl unsigned", ri16(0x0C1, 0x8000, 4), SPU_iohl, 0x8000);
    expect_imm("fsmbi", ri16(0x065, 0xFFFF, 4), SPU_fsmbi, 0xFFFF);

    /* RI16 targets: pc-relative forms add pc, absolute ones do not; the
     * quadword forms mask to 16-byte alignment, branches to word alignment. */
    expect_tgt("lqr back", ri16(0x067, 0xFFFF, 4), 0x104, SPU_lqr, 0x100);
    expect_tgt("stqr back", ri16(0x047, 0xFFFF, 4), 0x104, SPU_stqr, 0x100);
    expect_tgt("lqr wraps past 256K", ri16(0x067, 8, 4), 0x3FFF0, SPU_lqr, 0x10);
    expect_tgt("lqr wraps below 0", ri16(0x067, 0xFFF0, 4), 0x20, SPU_lqr, 0x3FFE0);
    expect_tgt("br wraps past 256K", ri16(0x064, 1, 0), 0x3FFFC, SPU_br, 0);
    expect_tgt("br wraps below 0", ri16(0x064, 0xFFFF, 0), 0, SPU_br, 0x3FFFC);
    expect_tgt("br forward", ri16(0x064, 0x10, 0), 0x200, SPU_br, 0x240);
    expect_tgt("brsl", ri16(0x066, 4, 0), 0x400, SPU_brsl, 0x410);
    expect_tgt("brz", ri16(0x040, 2, 3), 0x800, SPU_brz, 0x808);
    expect_tgt("brnz", ri16(0x042, 0xFFFE, 3), 0x800, SPU_brnz, 0x7F8);
    expect_tgt("brhz", ri16(0x044, 1, 3), 0x800, SPU_brhz, 0x804);
    expect_tgt("brhnz", ri16(0x046, 1, 3), 0x800, SPU_brhnz, 0x804);
    expect_tgt("bra ignores pc", ri16(0x060, 0x4000, 0), 0x3FFFC, SPU_bra, 0x10000);
    expect_tgt("bra negative", ri16(0x060, 0xFFFF, 0), 0x100, SPU_bra, 0x3FFFC);
    expect_tgt("brasl", ri16(0x062, 3, 0), 0x100, SPU_brasl, 0xC);
    expect_tgt("lqa", ri16(0x061, 0xFFFF, 4), 0x100, SPU_lqa, 0x3FFF0);
    expect_tgt("stqa", ri16(0x041, 0x0123, 4), 0x100, SPU_stqa, 0x0480);

    /* RI10: lqd/stqd scale the displacement by 16; the rest do not. */
    expect_imm("lqd +3", ri10(0x34, 3, 1, 2), SPU_lqd, 48);
    expect_imm("lqd -2", ri10(0x34, 0x3FE, 1, 2), SPU_lqd, -32);
    expect_imm("stqd -512", ri10(0x24, 0x200, 1, 2), SPU_stqd, -8192);
    expect_imm("stqd +511", ri10(0x24, 0x1FF, 1, 2), SPU_stqd, 8176);
    expect_imm("ai -1", ri10(0x1C, 0x3FF, 1, 2), SPU_ai, -1);
    expect_imm("ori +511", ri10(0x04, 0x1FF, 1, 2), SPU_ori, 511);
    {
        uint32_t i = ri10(0x1C, 0x155, 17, 90);
        spu_ins d = dec(i, 0);
        CHECK(d.ra == 17 && d.rt == 90 && d.ch == 17, "ai fields", i);
    }

    /* Channel ops are 11-bit opcodes; wrch (0x10D) must not be taken for the
     * shift-immediate family whose 11-bit opcode (shli 0x07B) differs. */
    {
        uint32_t i = ri7(0x00D, 0, 5, 6); /* rdch $6, $ch5 */
        spu_ins d = dec(i, 0);
        CHECK(d.op == SPU_rdch && d.ch == 5 && d.rt == 6 && d.imm == 0, "rdch", i);
        i = ri7(0x10D, 0, 7, 8);
        d = dec(i, 0);
        CHECK(d.op == SPU_wrch && d.ch == 7 && d.rt == 8 && d.imm == 0, "wrch", i);
        i = ri7(0x00F, 0, 9, 10);
        d = dec(i, 0);
        CHECK(d.op == SPU_rchcnt && d.ch == 9 && d.rt == 10 && d.imm == 0, "rchcnt", i);
        /* The whole 7-bit channel field reaches spu_rdch/spu_wrch; SPU channels go up to 127. */
        i = ri7(0x00D, 0, 0x7F, 1);
        d = dec(i, 0);
        CHECK(d.op == SPU_rdch && d.ch == 0x7F && d.rt == 1, "rdch channel 127", i);
        expect_imm("shli is not wrch", ri7(0x07B, 3, 5, 6), SPU_shli, 3);
    }

    /* STOP: 14-bit signal type, and no other RR form carries an immediate. */
    expect_imm("stop 14 bits", 0x00003FFFu, SPU_stop, 0x3FFF);
    expect_imm("stop 0x2A5", 0x000002A5u, SPU_stop, 0x2A5);
    expect_imm("stop ignores bit 14", 0x00004001u, SPU_stop, 1);
    expect_imm("a has no imm", ri7(0x0C0, 0x7F, 1, 2), SPU_a, 0);

    /* RI7: shifts/rotates sign-extend, the generate-controls zero-extend. */
    expect_imm("shli -64", ri7(0x07B, 0x40, 1, 2), SPU_shli, -64);
    expect_imm("roti 63", ri7(0x078, 0x3F, 1, 2), SPU_roti, 63);
    expect_imm("rotqbyi -1", ri7(0x1FC, 0x7F, 1, 2), SPU_rotqbyi, -1);
    expect_imm("cwd 64 unsigned", ri7(0x1F6, 0x40, 1, 2), SPU_cwd, 64);
    expect_imm("cbd 127 unsigned", ri7(0x1F4, 0x7F, 1, 2), SPU_cbd, 127);

    /* RI8: unsigned 8-bit scale, for all four conversions. */
    expect_imm("cflts 255", ri8(0x1D8, 0xFF, 1, 2), SPU_cflts, 255);
    expect_imm("cfltu 255", ri8(0x1D9, 0xFF, 1, 2), SPU_cfltu, 255);
    expect_imm("csflt 0x81", ri8(0x1DA, 0x81, 1, 2), SPU_csflt, 0x81);
    expect_imm("cuflt 0x80", ri8(0x1DB, 0x80, 1, 2), SPU_cuflt, 0x80);

    /* RR/LSX register forms. */
    {
        uint32_t i = ri7(0x1C4, 11, 12, 13); /* lqx $13, $12, $11 */
        spu_ins d = dec(i, 0);
        CHECK(d.op == SPU_lqx && d.rb == 11 && d.ra == 12 && d.rt == 13 && d.imm == 0 && d.tgt == 0,
              "lqx", i);
        i = ri7(0x144, 11, 12, 13);
        d = dec(i, 0);
        CHECK(d.op == SPU_stqx && d.rb == 11 && d.ra == 12 && d.rt == 13, "stqx", i);
    }

    /* A word no table claims traps as `.word`, with no immediate. */
    {
        uint32_t i = 0x00800000u; /* 11-bit opcode 0x004 */
        spu_ins d = dec(i, 0x40);
        CHECK(d.op == SPU_word && d.imm == 0 && d.tgt == 0, "invalid word", i);
    }

    /* dfceq, dfcmeq, dfcgt, dfcmgt and dftsv decode as the same trap: a CBE SPU raises an
     * invalid-instruction error for each (console probe, CECHC04 on FW 4.81: the thread group
     * ended with join cause 4 and the stopped thread's exit status read 0x8001000F), so
     * gen_spu_interp.py maps their RR prefixes to SPU_word even though spu_disasm.py still
     * names them. Every register field at its extreme proves the decode does not look at one.
     * The neighbouring DP op keeps its decode: dfa is 0x2CC. */
    {
        static const uint32_t op11[5] = { 0x3C3, 0x3CB, 0x2C3, 0x2CB, 0x3BF };
        for (unsigned k = 0; k < sizeof op11 / sizeof op11[0]; k++) {
            uint32_t i = (op11[k] << 21) | (0x7Fu << 14) | (0x7Fu << 7) | 0x7Fu;
            spu_ins d = dec(i, 0x40);
            CHECK(d.op == SPU_word && d.imm == 0 && d.tgt == 0, "CBE-invalid DP op", i);
            check_stops_at_word(op11[k]);
        }
        uint32_t i = ri7(0x2CC, 0x7F, 0x7F, 0x7F); /* dfa */
        spu_ins d = dec(i, 0);
        CHECK(d.op == SPU_dfa, "dfa still decodes", i);
    }

    /* cflts/cfltu at I8 <= 14 halt on a lane in the unmodelled region (sh = be + 23 - I8 >= 264,
     * spu_cfl_unmodelled_lane in spu_helpers.h; console probe, CECHC04 on FW 4.81) and write
     * nothing; every modelled lane runs, and I8 >= 15 never halts. Words: 0x3F800000 is 1.0
     * (modelled at every scale), 0x7F800000 / 0xFFFFFFFF are exponent-255 words of either sign,
     * 0x7F5BAACA / 0xFF66344D exponent 254. */
    {
        static const struct { uint32_t op10; uint32_t i8; uint32_t in[4]; int lane; uint32_t want[4]; } c[] = {
            /* halts: lane named, destination untouched */
            { 0x1D8, 14, { 0x3F800000, 0x7F800000, 0x3F800000, 0xFFFFFFFF }, 1, {0} },
            { 0x1D8, 0,  { 0x3F800000, 0x3F800000, 0x3F800000, 0xFFABDE3D }, 3, {0} },
            { 0x1D9, 13, { 0x3F800000, 0x7F800000, 0x7F5BAACA, 0xFFFFFFFF }, 1, {0} },
            { 0x1D9, 0,  { 0x79607710, 0x3F800000, 0x3F800000, 0x3F800000 }, 0, {0} },
            /* runs: sh 263 (both ops), sh 264 for cfltu (+ -> all ones, - -> 0) */
            { 0x1D8, 14, { 0x3F800000, 0x3F800000, 0x7F5BAACA, 0xFF66344D }, -1,
              { 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x80000000 } },
            { 0x1D9, 14, { 0x3F800000, 0x7F800000, 0x7F5BAACA, 0xFFFFFFFF }, -1,
              { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0 } },
            /* I8 15 cannot reach sh 264: an exponent-255 NaN lane converts (console: sh = 263) */
            { 0x1D8, 15, { 0x3F800000, 0x3F800000, 0x3F800000, 0xFFABDE3D }, -1,
              { 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x80000000 } },
        };
        for (unsigned k = 0; k < sizeof c / sizeof c[0]; k++) {
            const uint32_t word = ri8(c[k].op10, c[k].i8, 4, 3);
            memset(&run_ctx, 0, sizeof run_ctx);
            spu_context_init(&run_ctx, 0);
            run_ctx.image_id = -1;
            for (int l = 0; l < 4; l++) run_ctx.gpr[4]._u32[l] = c[k].in[l];
            put_word(0x100, word);                      /* cflts/cfltu r3,r4,I8 */
            put_word(0x104, ri16(0x081, 1, 5));         /* il r5,1: runs only if the word did */
            put_word(0x108, 0);                         /* stop 0 */
            (void)spu_interp_run(&run_ctx, 0x100);
            if (c[k].lane >= 0) {
                CHECK(run_ctx.status == SPU_STATUS_STOPPED_BY_HALT && run_ctx.pc == 0x100, "cfl halt at the word", word);
                CHECK(run_ctx.gpr[3]._u32[0] == 0 && run_ctx.gpr[5]._u32[0] == 0, "cfl wrote nothing", word);
            } else {
                CHECK(run_ctx.status == SPU_STATUS_STOPPED_BY_STOP && run_ctx.gpr[5]._u32[0] == 1, "cfl ran on", word);
                CHECK(!memcmp(run_ctx.gpr[3]._u32, c[k].want, 16), "cfl modelled lanes", word);
            }
        }
    }

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("spu_decode1: all contract checks passed\n");
    return 0;
}

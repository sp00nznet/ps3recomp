/* spu_interp_selftest.c — end-to-end check of the SPU interpreter.
 *
 * Runs a snippet assembled by spu-lv2-as (bytes below) through the interpreter
 * and asserts the architectural results. Proves decode + execute + control flow
 * + local-store store, using the same helpers the lifter emits.
 *
 *   il $2,5; il $3,7; a $4,$2,$3; ai $4,$4,100; rotqbyi $5,$4,0;
 *   sf $6,$2,$3; stqd $4,0($1); stop
 */
#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* This snippet issues no channel ops; stub the channel ABI so the test links
 * without the full runtime (spu_channels.c pulls in unrelated globals). */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c;(void)ch; u128 z; memset(&z,0,sizeof z); return z; }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c;(void)ch;(void)v; }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c;(void)ch; return 1; }

/* The rest of the runtime the interpreter reaches: probes off, nothing lifted. */
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch, g_spu_ls_dbg;
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c;(void)a; return 0; }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; }
void spu_ls_watch_slow(const spu_context* c, uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) { (void)c;(void)l;(void)w;(void)p;(void)pc;(void)lr; }
/* more of the runtime surface spu_interp.c references (kept inert here) */
void (*g_spu_lv2_stop_hook)(spu_context*, uint32_t) = 0;
void spu_drain_call(spu_context* c, uint32_t lr) { (void)c; (void)lr; }
void spu_img_restore(spu_context* c, int32_t img) { (void)c; (void)img; }
void spu_indirect_branch(spu_context* c) { c->status = SPU_STATUS_STOPPED_BY_HALT; }
void spu_ls_watch_dump(const char* why) { (void)why; }
void spu_trace_pc(spu_context* c, uint32_t pc) { (void)c; (void)pc; }
uint8_t* vm_base = 0;

/* .text of the assembled snippet (big-endian words, verbatim). */
static const unsigned char PROG[] = {
    0x40,0x80,0x02,0x82, 0x40,0x80,0x03,0x83, 0x18,0x00,0xc1,0x04,
    0x1c,0x19,0x02,0x04, 0x3f,0x80,0x02,0x05, 0x08,0x00,0xc1,0x06,
    0x24,0x00,0x00,0x84, 0x00,0x00,0x00,0x00,
};

int main(void) {
    static spu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    spu_context_init(&ctx, 0);          /* ls points at ls_store */
    memcpy(ctx.ls, PROG, sizeof PROG);
    ctx.gpr[1]._u32[0] = 0x100;   /* stack pointer (store target base) */

    spu_interp_run(&ctx, 0);

    #define P(i) (ctx.gpr[i]._u32[0])
    printf("r2=%u r3=%u r4=%u r5=%u r6=%u stop=0x%X\n",
           P(2), P(3), P(4), P(5), P(6), ctx.stop_code);

    assert(P(2) == 5);
    assert(P(3) == 7);
    assert(P(4) == 112);          /* 5+7+100 */
    assert(P(5) == 112);          /* rotqbyi 0 = identity */
    assert(P(6) == 2);            /* sf: b - a = 7 - 5 */
    assert(ctx.status == SPU_STATUS_STOPPED_BY_STOP);

    /* stqd $4,0($1): LS[0x100] holds 112 big-endian in the preferred word. */
    const unsigned char* q = &ctx.ls[0x100];
    unsigned w = ((unsigned)q[0]<<24)|((unsigned)q[1]<<16)|((unsigned)q[2]<<8)|q[3];
    assert(w == 112);

    /* --- backward-branch loop (the SPU counter-loop mechanism) via spu_dispatch --
     * il $2,0; il $3,5; loop: ai $2,$2,1; ceq $4,$2,$3; brz $4,loop; stop */
    static const unsigned char LOOP[] = {
        0x40,0x80,0x00,0x02, 0x40,0x80,0x02,0x83, 0x1c,0x00,0x41,0x02,
        0x78,0x00,0xc1,0x04, 0x20,0x7f,0xff,0x04, 0x00,0x00,0x00,0x00,
    };
    static spu_context lc;
    memset(&lc, 0, sizeof lc);
    spu_context_init(&lc, 0);
    memcpy(lc.ls, LOOP, sizeof LOOP);
    spu_dispatch(&lc, 0);                 /* enters via the computed-branch entry point */
    printf("loop r2=%u stop=0x%X\n", lc.gpr[2]._u32[0], lc.stop_code);
    assert(lc.gpr[2]._u32[0] == 5);       /* loop terminated at the bound */
    assert(lc.status == SPU_STATUS_STOPPED_BY_STOP);

    /* --- a drain's return point is a rejoin point: il $2,5 then il $3,7 at
     * stop at 4 -- the interpreter must stop before the second il. */
    static spu_context dc;
    memset(&dc, 0, sizeof dc);
    spu_context_init(&dc, 0);
    memcpy(dc.ls, PROG, sizeof PROG);
    spu_interp_run_until(&dc, 0, 4);
    assert(dc.pc == 4 && dc.gpr[2]._u32[0] == 5 && dc.gpr[3]._u32[0] == 0);

    /* --- T-0001 regression: the inFamous WWS scatter loop (image
     * spu_0003_at_006A3300 0x12D80/0x12E40), interpreter side. The same
     * encoding runs lifted in run_tests.sh (test_loop_bounds); here the
     * interpreter must agree: exact iteration count, records confined to
     * the scratch span, nothing else in the 256 KB touched. Parameters in
     * r4 (base) r5 (count) r8/r9 (shifts); r0 = exit stub. See
     * gen_test_loop_bounds.py for the instruction-by-instruction source. */
    {
        static const unsigned char T0001_LOOP[] = {
            /* faithful to the real guest bytes (image 4, LS 0x12D80 +
             * 0x12E40), including the head's shlqbii x4 pair and the
             * r14=0 prologue (set 0 is stored twice); see
             * gen_test_loop_bounds.py for the instruction listing. */
0x40,0x80,0x08,0x0B,0x0F,0x61,0x02,0x82,0x0B,0x62,0x01,0x02,0x3B,0x62,0x05,0x88,
            0x0C,0x00,0x01,0x02,0x3B,0x62,0x45,0x89,0x18,0x02,0x42,0x13,0x18,0x02,0x49,0x97,
            0x18,0x02,0x4B,0x9B,0x3F,0x60,0x84,0x08,0x3F,0x60,0x84,0x89,0x40,0x80,0x00,0x0C,
            0x40,0x80,0x00,0x0E,0x40,0x89,0x9B,0x8D,0x40,0x9A,0x1A,0x22,0x40,0x9A,0x9A,0xA3,
            0x40,0x9B,0x1B,0x24,0x42,0x00,0x40,0x0F,0x32,0x00,0x07,0x00,0x00,0x20,0x00,0x00,
            0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x00,
            0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x00,
            0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x00,
            0x7C,0x00,0x01,0x25,0x28,0x83,0x02,0x0D,0x18,0x00,0x84,0x02,0x28,0x83,0x09,0xA2,
            0x18,0x02,0x45,0x0A,0x28,0x83,0x0B,0xA3,0x28,0x83,0x0D,0xA4,0x84,0xA0,0x07,0xA5,
            0x18,0x03,0x86,0x0C,0x1C,0x00,0x04,0x8E,0x35,0x00,0x12,0x80,0x00,0x20,0x00,0x00,
            0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x00,
            0x00,0x00,0x00,0x00,
        };
        enum { T0001_BODY = 0x80, T0001_EXIT = 0xC0, T0001_BASE = 0x8000 };
        static const struct { uint32_t cnt, sa, sb; } cases[] = {
            {32,0,0}, {128,0,0}, {4,0,0}, {128,3,3},
        };
        for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
            static spu_context tc;   /* static: the 256 KB LS */
            memset(&tc, 0, sizeof tc);
            spu_context_init(&tc, 0);
            memcpy(tc.ls, T0001_LOOP, sizeof T0001_LOOP);
            memset(tc.ls + 0x1000, 0xA5, 0x40000 - 0x1000);   /* sentinel */
            tc.gpr[0]._u32[0] = T0001_EXIT;
            tc.gpr[1]._u32[0] = 0x3F000;
            tc.gpr[4]._u32[0] = T0001_BASE;
            tc.gpr[5]._u32[0] = cases[k].cnt;
            tc.gpr[8]._u32[0] = cases[k].sa;
            tc.gpr[9]._u32[0] = cases[k].sb;
            spu_dispatch(&tc, 0);
            uint32_t s = cases[k].sb & 7;
            uint32_t stride = 16u << s;
            /* exit round stores too; set 0 double-stored; contiguous
             * (stride 16) top line = (cnt-1)*stride + 16 */
            uint32_t hi = T0001_BASE + stride * (cases[k].cnt - 1) + 16;
            assert(tc.status == SPU_STATUS_STOPPED_BY_STOP);
            for (uint32_t a = 0x1000; a < 0x40000; a += 16) {
                int changed = tc.ls[a] != 0xA5;
                int inside = a >= T0001_BASE && a < hi;
                if (!inside) assert(!changed);           /* T-0001 flood signature */
                if (inside && stride == 16 && a < hi - 64) assert(changed); /* full span */
            }
            printf("t0001 interp cnt=%u sa=%u sb=%u: OK (%u records confined)\n",
                   cases[k].cnt, cases[k].sa, cases[k].sb, cases[k].cnt);
        }
    }

    /* --- T-0001 REAL BYTES: the actual guest code (image spu_0003_at_006A3300,
     * LS 0x12D80 head + 0x12E40 body through the closing bi at 0x12EAC),
     * extracted from the game binary. Ground truth for the transcription
     * above: the real loop must (a) exit iff cnt % 4 == 0 (the head's
     * shlqbii x4 step makes the counter advance 4x the unit), (b) write
     * EXACTLY cnt records into the predicted span, (c) nothing anywhere
     * else. Counts not divisible by 4 NEVER exit -- the boot6/8 hang/crash
     * family -- so those run under interp_step_budget and must show the
     * flood signature: stores outside the span. */
    {
        static const unsigned char T0001_REAL[] = {
0x0F,0x61,0x02,0x82,0x3F,0xE4,0x01,0x8A,0x40,0x80,0x08,0x0B,0x3F,0xE4,0x01,0x8C,
            0x42,0xEB,0x48,0x0D,0x00,0x20,0x00,0x00,0x40,0x20,0x00,0x02,0x3F,0xE4,0x01,0x8E,
            0x0B,0x62,0x01,0x02,0x3B,0x62,0x05,0x88,0x42,0x97,0x20,0x0F,0x3B,0x62,0x45,0x89,
            0x40,0x20,0x00,0x7F,0x12,0x00,0x11,0xBE,0x40,0x20,0x00,0x0C,0x34,0x00,0x06,0x8B,
            0x18,0x00,0x81,0x90,0x34,0x00,0x46,0x83,0x0C,0x00,0x01,0x02,0x34,0x00,0x86,0x8D,
            0x40,0x20,0x00,0x7F,0x00,0x20,0x00,0x00,0x18,0x04,0x04,0x11,0x38,0x80,0x88,0x12,
            0x18,0x01,0x04,0x93,0x38,0x82,0x82,0x14,0x18,0x02,0x08,0x95,0x38,0x80,0x88,0x96,
            0x18,0x04,0xC4,0x97,0x38,0x82,0x89,0x98,0x18,0x02,0x0A,0x99,0x38,0x80,0x8A,0x9A,
            0x18,0x02,0x4B,0x9B,0x38,0x82,0x8B,0x9C,0x1C,0x00,0x03,0x1D,0x38,0x80,0x8C,0x9E,
            0x58,0x81,0x83,0x9F,0x38,0x82,0x8D,0xA0,0xE1,0x62,0xC3,0x86,0x3F,0x60,0x84,0x08,
            0xE4,0x20,0xC3,0x86,0x3F,0x60,0x84,0x89,0x58,0xC1,0xC6,0x87,0x00,0x20,0x00,0x00,
            0x40,0x20,0x00,0x00,0x00,0x20,0x00,0x00,0x40,0x20,0x00,0x00,0x00,0x20,0x00,0x00,
            0x7C,0x00,0x01,0x25,0x28,0x83,0x02,0x0D,0x18,0x00,0x84,0x02,0x28,0x83,0x09,0xA2,
            0x18,0x02,0x45,0x0A,0x28,0x83,0x0B,0xA3,0xE1,0xA7,0x49,0x14,0x28,0x83,0x0D,0xA4,
            0xE4,0x45,0x8F,0x98,0x3F,0xE0,0x0E,0x83,0xE4,0x62,0xCD,0x1C,0x38,0x80,0x88,0x12,
            0xE4,0x87,0x90,0xA0,0x38,0x82,0x82,0x14,0x58,0x81,0xCE,0x9D,0x38,0x80,0x88,0x96,
            0x58,0x81,0xCF,0x9F,0x38,0x82,0x89,0x98,0x58,0x82,0xC3,0x8B,0x38,0x80,0x8A,0x9A,
            0x58,0x81,0xD0,0xA1,0x38,0x82,0x8B,0x9C,0x84,0xA0,0x07,0xA5,0x38,0x80,0x8C,0x9E,
            0x18,0x03,0x07,0x0C,0x38,0x82,0x8D,0xA0,0x1C,0x00,0x04,0x8E,0x35,0x00,0x12,0x80,
        };
        enum { R_HEAD = 0x12D80, R_EXIT = 0x1000, R_BASE = 0x8000, R_INPUT = 0x2A000 };
        static const struct { uint32_t cnt, sa, sb; int exits; } rcases[] = {
            {32,0,0,1}, {128,0,0,1}, {4,0,0,1}, {128,3,3,1},   /* good traffic */
            {5834,0,0,0}, {607,0,0,0}, {1,0,0,0},              /* cnt%4!=0: never exits */
            {128,8,8,0},                                       /* shift>7: masking overshoot */
        };
        for (unsigned k = 0; k < sizeof rcases / sizeof rcases[0]; k++) {
            static spu_context tc;   /* static: the 256 KB LS */
            memset(&tc, 0, sizeof tc);
            spu_context_init(&tc, 0);
            memset(tc.ls + 0x1010, 0xA5, 0x40000 - 0x1010);   /* sentinel FIRST ... */
            memcpy(tc.ls + R_HEAD, T0001_REAL, sizeof T0001_REAL);  /* ... then code */
            memset(tc.ls + R_EXIT, 0, 16);                     /* the stop stub */
            tc.gpr[0]._u32[0] = R_EXIT;
            tc.gpr[1]._u32[0] = 0x3F000;
            tc.gpr[3]._u32[0] = R_INPUT;      /* input stream base (consumed into r16) */
            tc.gpr[4]._u32[0] = R_BASE;       /* record base */
            tc.gpr[5]._u32[0] = rcases[k].cnt;
            tc.gpr[6]._u32[0] = 0x3F800000;   /* scale seed (fma path: value-irrelevant) */
            tc.gpr[7]._u32[0] = 0x40000000;
            tc.gpr[8]._u32[0] = rcases[k].sa;
            tc.gpr[9]._u32[0] = rcases[k].sb;
            uint32_t stride = 16u << (rcases[k].sb & 7);
            uint32_t hi = R_BASE + stride * (rcases[k].cnt - 1) + 16;
            if (rcases[k].exits) {
                spu_dispatch(&tc, R_HEAD);
                assert(tc.status == SPU_STATUS_STOPPED_BY_STOP);
            } else {
                tc.interp_step_budget = 200000;   /* hang families: bounded run */
                spu_interp_run_until(&tc, R_HEAD, R_EXIT);
            }
            uint32_t outside = 0, inside = 0;
            for (uint32_t a = 0x1010; a < 0x40000; a += 16) {
                if (a >= R_HEAD && a < R_HEAD + sizeof T0001_REAL) continue; /* program bytes */
                int changed = tc.ls[a] != 0xA5;
                if (a >= R_BASE && a < hi) { if (changed) inside++; }
                else if (changed) outside++;
            }
            if (rcases[k].exits) {
                /* exact record count, zero collateral damage */
                assert(inside == rcases[k].cnt);
                assert(outside == 0);
                printf("t0001 real-bytes cnt=%u sa=%u sb=%u: OK (exited, %u records exact, no flood)\n",
                       rcases[k].cnt, rcases[k].sa, rcases[k].sb, inside);
            } else {
                /* The T-0001 anomaly, caught on the real bytes. Any of:
                 * (a) still running at the budget (count % 4 != 0 -- the
                 *     counter can NEVER reach zero, not even via the 2^32
                 *     wrap: the boot8 black-screen hang),
                 * (b) dead by SELF-OVERWRITE -- the stores march around the
                 *     whole LS and reach the loop's own code, and the SPU
                 *     executes the record bytes as instructions (exactly
                 *     what kills the job instantly on real hardware; the
                 *     lifted runtime hides this and keeps flooding -- the
                 *     boot6 crash),
                 * (c) exited with out-of-span damage (shift>7 masking
                 *     overshoot).
                 * A clean, confined, exactly-cnt exit would mean the contract
                 * broke and T-0001 would silently regress. */
                int clean = (tc.status == SPU_STATUS_STOPPED_BY_STOP)
                            && inside == rcases[k].cnt && outside == 0;
                assert(!clean);
                printf("t0001 real-bytes cnt=%u sa=%u sb=%u: ANOMALY as predicted "
                       "(status=%u inside=%u outside=%u)\n",
                       rcases[k].cnt, rcases[k].sa, rcases[k].sb, tc.status, inside, outside);
            }
        }
    }

    /* --- T-0001 IRQ regression (2026-10-08 root cause): the interrupt
     * register-save/restore contract on the INTERPRETER path. The WWS
     * jobmanager's iret executes interpreted (evicted/re-streamed code);
     * before the fix, the interpreter never checked irq_saved -- the
     * 128-GPR snapshot was silently stranded and later false-fired on
     * the LLE thread, memcpy'ing interrupt-time registers over live job
     * parameters (the once-per-boot corruption: scatter flood / C798
     * flood / branch-to-0 hang / wander crash). These fixtures pin the
     * fixed contract instruction by instruction.
     *
     * Encodings (verified against spu_decode1 / spu_interp_tables.inc):
     *   iret  = 0x35400000 (op11 0x1AA); irete |= 0x40000; iretd |= 0x80000
     *   bi    = 0x35000000 | (rA << 7);  bi with E bit |= 0x40000
     *   bisled= 0x35600000 | (rA << 7);  E bit when TAKEN
     *   il rt,imm = 0x40800000 | (imm << 7) | rt; stop = 0x00000000 */
    {
        #define W(a, w) do { ic.ls[(a)+0]=(unsigned char)((w)>>24); ic.ls[(a)+1]=(unsigned char)((w)>>16); \
                             ic.ls[(a)+2]=(unsigned char)((w)>>8);  ic.ls[(a)+3]=(unsigned char)(w); } while (0)
        static spu_context ic;

        /* (1) genuine iret to the take point: snapshot restored, armed
         *     flag cleared, marker register revived from the snapshot
         *     while execution continues at the resume pc. */
        memset(&ic, 0, sizeof ic);
        spu_context_init(&ic, 0);
        W(0x00, 0x35400000u);                  /* iret            */
        W(0x20, 0x40800000u | (0x5A5Au << 7) | 2);  /* il $2,0x5A5A   */
        W(0x24, 0x00000000u);                  /* stop            */
        ic.srr0 = 0x20;
        ic.int_enable = 0;
        ic.irq_saved = 1; ic.irq_resume_pc = 0x20; ic.irq_save_steps = 0x1000;
        ic.irq_gpr[7]._u32[0] = 0xDEAD;
        ic.gpr[7]._u32[0] = 0xBEEF;             /* handler "clobbered" live state */
        spu_interp_run_until(&ic, 0, 0);
        assert(ic.irq_saved == 0);                       /* snapshot consumed  */
        assert(ic.gpr[7]._u32[0] == 0xDEAD);              /* restored, not live */
        assert(ic.gpr[2]._u32[0] == 0x5A5A);              /* ran the resume pc */
        assert(ic.status == SPU_STATUS_STOPPED_BY_STOP);
        printf("t0001-irq interp iret: restored (r7=%08X, armed cleared)\n",
               ic.gpr[7]._u32[0]);

        /* (2) iret to a MISMATCHED pc: snapshot must stay armed and the
         *     live registers untouched -- this is the anti-false-fire
         *     guarantee (restore only at the genuine take point). */
        memset(&ic, 0, sizeof ic);
        spu_context_init(&ic, 0);
        W(0x00, 0x35400000u);                  /* iret -> srr0=0x30 */
        W(0x30, 0x40800000u | (0x1234u << 7) | 2);  /* il $2,0x1234   */
        W(0x34, 0x00000000u);
        ic.srr0 = 0x30;                         /* != irq_resume_pc */
        ic.int_enable = 0;
        ic.irq_saved = 1; ic.irq_resume_pc = 0x20; ic.irq_save_steps = 0x1000;
        ic.irq_gpr[7]._u32[0] = 0xDEAD;
        ic.gpr[7]._u32[0] = 0xBEEF;
        spu_interp_run_until(&ic, 0, 0);
        assert(ic.irq_saved == 1);                       /* still armed      */
        assert(ic.gpr[7]._u32[0] == 0xBEEF);              /* live preserved   */
        assert(ic.gpr[2]._u32[0] == 0x1234);
        printf("t0001-irq mismatch iret: snapshot stays armed, no false fire\n");

        /* (3) bi-family E/D bits, previously dropped by the interpreter:
         *     (3a) bi with the E bit enables interrupts;
         *     (3b) iretd disables them. */
        memset(&ic, 0, sizeof ic);
        spu_context_init(&ic, 0);
        W(0x00, 0x35000000u | (3u << 7) | 0x40000u);  /* bie $3 -> gpr[3]  */
        W(0x20, 0x00000000u);                  /* stop at target */
        ic.gpr[3]._u32[0] = 0x20;
        ic.int_enable = 0;
        spu_interp_run_until(&ic, 0, 0);
        assert(ic.int_enable == 1);
        assert(ic.status == SPU_STATUS_STOPPED_BY_STOP);
        printf("t0001-irq bi E-bit: int_enable set when taken\n");

        memset(&ic, 0, sizeof ic);
        spu_context_init(&ic, 0);
        W(0x00, 0x35400000u | 0x80000u);       /* iretd           */
        W(0x20, 0x00000000u);
        ic.srr0 = 0x20;
        ic.int_enable = 1;
        spu_interp_run_until(&ic, 0, 0);
        assert(ic.int_enable == 0);
        printf("t0001-irq iret D-bit: int_enable cleared\n");

        /* (4) bisled taken with the E bit: event pending -> branch +
         *     int_enable. spu_ev_get is a macro over event_status. */
        memset(&ic, 0, sizeof ic);
        spu_context_init(&ic, 0);
        W(0x00, 0x35600000u | (3u << 7) | 0x40000u);  /* bislede $3 */
        W(0x20, 0x00000000u);
        ic.gpr[3]._u32[0] = 0x20;
        ic.int_enable = 0;
        ic.event_status = 1; ic.event_mask = 1;         /* event pending */
        spu_interp_run_until(&ic, 0, 0);
        assert(ic.int_enable == 1);
        assert(ic.status == SPU_STATUS_STOPPED_BY_STOP);
        printf("t0001-irq bisled E-bit: taken, int_enable set\n");
        #undef W
    }

    printf("spu_interp_selftest: PASS\n");
    return 0;
}

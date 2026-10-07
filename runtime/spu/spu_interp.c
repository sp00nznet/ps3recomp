/* spu_interp.c — SPU interpreter core. See spu_interp.h.
 *
 * Decode tables are generated from the validated Python decoder
 * (tools/gen_spu_interp.py -> spu_interp_tables.inc); execute calls the SAME
 * spu_<mnemonic> helpers the lifter emits, so interpreted and lifted code are
 * bit-identical. Uncommon opcodes trap loudly (never silently wrong).
 */
#include "spu_interp.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

/* Channel ABI (runtime/spu/spu_channels.c); the lifter emits these same protos
 * into each generated spu_recomp.h — declared here to stay header-independent. */
u128 spu_rdch(spu_context* ctx, uint32_t channel);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel);

#include "spu_interp_tables.inc"   /* spu_op enum, spu_op_name[], spu_dec_* */

/* Work descriptor for the next SYS_SPU_THREAD_STOP_RECEIVE_EVENT service, set
 * by the event-port send that wakes a sim SPU. Single-slot: dispatch is
 * synchronous -- the sending PPU thread runs the SPU inline. Thread-local for
 * exactly that reason: a global let an SPU running on its own host thread take
 * whatever event a PPU had just staged, ahead of the ones queued before it
 * (and the event was still queued as well) -- the mc suite's recv case saw its
 * second and third events arrive swapped. Only the inline re-run on the
 * sending thread can see the slot; every other SPU receives from its queue. */
SPU_THREAD_LOCAL uint32_t g_spu_pending_evt[3];
SPU_THREAD_LOCAL int      g_spu_pending_evt_valid;

/* ---- decode: 32-bit insn -> fields (mirrors spu_disasm.spu_decode order) ---- */
typedef struct {
    spu_op   op;
    uint8_t  rt, ra, rb, rc, ch;
    int32_t  imm;     /* sign/zero-extended immediate or shift, per format */
    uint32_t tgt;     /* absolute LS branch target (br-family) */
} spu_ins;

static int32_t sx(uint32_t v, int bits) {
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((v ^ m) - m);
}

static void spu_decode1(uint32_t insn, uint32_t pc, spu_ins* d) {
    uint32_t op4 = (insn >> 28) & 0xF,  op7 = (insn >> 25) & 0x7F;
    uint32_t op8 = (insn >> 24) & 0xFF, op9 = (insn >> 23) & 0x1FF;
    uint32_t op10 = (insn >> 22) & 0x3FF, op11 = (insn >> 21) & 0x7FF;
    d->rt = insn & 0x7F; d->ra = (insn >> 7) & 0x7F;
    d->rb = (insn >> 14) & 0x7F; d->rc = (insn >> 21) & 0x7F;
    d->ch = (insn >> 7) & 0x7F; d->imm = 0; d->tgt = 0;
    int32_t i10 = sx((insn >> 14) & 0x3FF, 10);
    int32_t i16 = sx((insn >> 7) & 0xFFFF, 16);
    uint32_t i18 = (insn >> 7) & 0x3FFFF;

    /* RRR (4) */
    if ((d->op = spu_dec_rrr(op4)) != SPU_word) return;
    /* RI18 (7) */
    if ((d->op = spu_dec_ri18(op7)) != SPU_word) {
        d->imm = (d->op == SPU_ila) ? (int32_t)i18 : (int32_t)(i18 & 0xFFFF);
        return;
    }
    /* RI16 (9) — before RI10 (shared top-8-bit opcodes) */
    if ((d->op = spu_dec_ri16(op9)) != SPU_word) {
        switch (d->op) {
        case SPU_lqa: case SPU_stqa: d->tgt = ((uint32_t)(i16 & 0xFFFF) << 2) & 0x3FFF0; break;
        case SPU_lqr: case SPU_stqr: d->tgt = ((uint32_t)(i16 * 4) + pc) & 0x3FFF0; break;
        case SPU_br: case SPU_brsl: case SPU_brz: case SPU_brnz:
        case SPU_brhz: case SPU_brhnz: d->tgt = ((uint32_t)(i16 * 4) + pc) & 0x3FFFF; break;
        case SPU_bra: case SPU_brasl: d->tgt = ((uint32_t)(i16 * 4)) & 0x3FFFF; break;
        case SPU_il: d->imm = i16; break;
        default: d->imm = (int32_t)(i16 & 0xFFFF); break; /* ilh/ilhu/iohl */
        }
        return;
    }
    /* channel ops (op11) — before RI10 (wrch/shli op8 clash) */
    if (op11 == 0x00D) { d->op = SPU_rdch;   return; }
    if (op11 == 0x10D) { d->op = SPU_wrch;   return; }
    if (op11 == 0x00F) { d->op = SPU_rchcnt; return; }
    /* RI10 (8) */
    if ((d->op = spu_dec_ri10(op8)) != SPU_word) {
        d->imm = (d->op == SPU_lqd || d->op == SPU_stqd) ? (i10 << 4) : i10;
        return;
    }
    /* RI8 (10) float<->int conversions */
    if ((d->op = spu_dec_ri8(op10)) != SPU_word) { d->imm = (insn >> 14) & 0xFF; return; }
    /* RR (11) */
    if ((d->op = spu_dec_rr(op11)) != SPU_word) {
        if (d->op == SPU_stop) d->imm = insn & 0x3FFF;
        return;
    }
    /* RI7 (11) rotate/shift/gen-control immediates */
    if ((d->op = spu_dec_ri7(op11)) != SPU_word) {
        d->imm = spu_ri7_unsigned(d->op) ? d->rb : sx(d->rb, 7);
        return;
    }
    /* LSX (redundant with RR's lqx/stqx, kept for order-parity) */
    if ((d->op = spu_dec_lsx(op11)) != SPU_word) return;
    d->op = SPU_word;
}

/* spu_lifted_lookup() is defined by spu_fn_registry.c (the single owner of the
 * lifted-function table + eviction). Declared in spu_interp.h. */

/* ---- execute one instruction; returns 1 if the SPU stopped ---- */
#define A   (ctx->gpr[d.ra])
#define B   (ctx->gpr[d.rb])
#define T   (ctx->gpr[d.rt])          /* also the 3rd source in RRR */
#define DST (ctx->gpr[d.rt])          /* default destination */
#define DSTC (ctx->gpr[d.rc])         /* RRR destination */
#define I   (d.imm)
#define PREF(r) ((r)._u32[0])

static int spu_step(spu_context* ctx) {
    uint32_t pc = ctx->pc, m = pc & SPU_LS_MASK;
    const uint8_t* p = ctx->ls;
    uint32_t insn = ((uint32_t)p[m] << 24) | ((uint32_t)p[m+1] << 16)
                  | ((uint32_t)p[m+2] << 8) | p[m+3];
    /* Decode once per LS word. A decode is a function of (word, pc) alone, so
     * a per-thread cache indexed by pc and validated against the word is
     * right for every context and image the thread runs, and code a DMA or
     * overlay replaces simply re-decodes. */
    typedef struct { uint32_t insn; uint32_t valid; spu_ins d; } spu_dent;
    static _Thread_local spu_dent* dc;
    if (!dc) dc = (spu_dent*)calloc(0x10000, sizeof *dc);
    spu_ins d;
    if (dc) {
        spu_dent* e = &dc[(m >> 2) & 0xFFFF];
        if (!e->valid || e->insn != insn) { spu_decode1(insn, pc, &e->d); e->insn = insn; e->valid = 1; }
        d = e->d;
    } else {
        spu_decode1(insn, pc, &d);
    }
    uint32_t next = (pc + 4) & 0x3FFFC;
    uint32_t call_link = 0;   /* set by a taken branch-and-link: the return address */

    switch (d.op) {
    /* immediates / loads */
    case SPU_il:  DST = spu_il(I); break;
    case SPU_ilh: DST = spu_ilh((uint16_t)I); break;   /* halfword splat, NOT a word splat */
    case SPU_ilhu: DST = spu_splat_u32((uint32_t)I << 16); break;
    case SPU_ila:  DST = spu_splat_u32((uint32_t)I & 0x3FFFF); break;
    case SPU_iohl: DST = spu_ori(DST, (int32_t)((uint32_t)I & 0xFFFF)); break;
    case SPU_fsmbi: DST = spu_fsmbi(I); break;
    case SPU_lqd: case SPU_lqx: case SPU_lqa: case SPU_lqr: {
        uint32_t a = (d.op==SPU_lqd) ? PREF(A)+ (uint32_t)I
                   : (d.op==SPU_lqx) ? PREF(A)+PREF(B) : d.tgt;
        DST = spu_ls_read128(ctx, a); break; }
    case SPU_stqd: case SPU_stqx: case SPU_stqa: case SPU_stqr: {
        uint32_t a = (d.op==SPU_stqd) ? PREF(A)+ (uint32_t)I
                   : (d.op==SPU_stqx) ? PREF(A)+PREF(B) : d.tgt;
        spu_ls_write128(ctx, a, DST); break; }
    /* integer arithmetic */
    case SPU_a:  DST = spu_a(A,B); break;
    case SPU_ai: DST = spu_ai(A,I); break;
    case SPU_sf: DST = spu_sf(A,B); break;
    case SPU_sfi:DST = spu_sfi(A,I); break;
    case SPU_ah: DST = spu_ah(A,B); break;
    case SPU_ahi:DST = spu_ahi(A,I); break;
    case SPU_sfh:DST = spu_sfh(A,B); break;
    case SPU_sfhi:DST = spu_sfhi(A,I); break;
    case SPU_addx: DST = spu_addx(A,B,DST); break;
    case SPU_sfx:  DST = spu_sfx(A,B,DST); break;
    case SPU_cg:   DST = spu_cg(A,B); break;
    case SPU_cgx:  DST = spu_cgx(A,B,DST); break;
    case SPU_mpyi: DST = spu_mpyi(A,I); break;
    case SPU_mpyui: DST = spu_mpyui(A,I); break;
    /* logical */
    case SPU_and: DST = spu_and(A,B); break;
    case SPU_or:  DST = spu_or(A,B); break;
    case SPU_xor: DST = spu_xor(A,B); break;
    case SPU_nand:DST = spu_nand(A,B); break;
    case SPU_nor: DST = spu_nor(A,B); break;
    case SPU_andc:DST = spu_andc(A,B); break;
    case SPU_orc: DST = spu_orc(A,B); break;
    case SPU_eqv: DST = spu_eqv(A,B); break;
    case SPU_andi:DST = spu_andi(A,I); break;
    case SPU_ori: DST = spu_ori(A,I); break;
    case SPU_xori:DST = spu_xori(A,I); break;
    case SPU_andbi:DST = spu_andbi(A,I); break;
    case SPU_andhi:DST = spu_andhi(A,I); break;
    case SPU_orhi: DST = spu_orhi(A,I); break;
    case SPU_orbi: DST = spu_orbi(A,I); break;
    case SPU_xorhi:DST = spu_xorhi(A,I); break;
    case SPU_xorbi:DST = spu_xorbi(A,I); break;
    case SPU_orx:  DST = spu_orx(A); break;
    /* compares */
    case SPU_ceq: DST = spu_ceq(A,B); break;
    case SPU_ceqh:DST = spu_ceqh(A,B); break;
    case SPU_ceqb:DST = spu_ceqb(A,B); break;
    case SPU_ceqi:DST = spu_ceqi(A,I); break;
    case SPU_cgt: DST = spu_cgt(A,B); break;
    case SPU_cgth:DST = spu_cgth(A,B); break;
    case SPU_cgtb:DST = spu_cgtb(A,B); break;
    case SPU_cgti:DST = spu_cgti(A,I); break;
    case SPU_clgt: DST = spu_clgt(A,B); break;
    case SPU_clgth:DST = spu_clgth(A,B); break;
    case SPU_clgtb:DST = spu_clgtb(A,B); break;
    case SPU_clgti:DST = spu_clgti(A,I); break;
    /* shifts / rotates */
    case SPU_shl:  DST = spu_shl(A,B); break;
    case SPU_shli: DST = spu_shli(A,I); break;
    case SPU_shlh: DST = spu_shlh(A,B); break;
    case SPU_shlhi:DST = spu_shlhi(A,I); break;
    case SPU_rot:  DST = spu_rot(A,B); break;
    case SPU_roti: DST = spu_roti(A,I); break;
    case SPU_rotm: DST = spu_rotm(A,B); break;
    case SPU_rotmi:DST = spu_rotmi(A,I); break;
    case SPU_rotma:DST = spu_rotma(A,B); break;
    case SPU_rotmai:DST = spu_rotmai(A,I); break;
    case SPU_roth: DST = spu_roth(A,B); break;
    case SPU_rothi:DST = spu_rothi(A,I); break;
    case SPU_shlqbi: DST = spu_shlqbi(A,B); break;
    case SPU_shlqbii:DST = spu_shlqbii(A,I); break;
    case SPU_shlqby: DST = spu_shlqby(A,B); break;
    case SPU_shlqbyi:DST = spu_shlqbyi(A,I); break;
    case SPU_rotqbi: DST = spu_rotqbi(A,B); break;
    case SPU_rotqbii:DST = spu_rotqbii(A,I); break;
    case SPU_rotqby: DST = spu_rotqby(A,B); break;
    case SPU_rotqbyi:DST = spu_rotqbyi(A,I); break;
    case SPU_rotqmby: DST = spu_rotqmby(A,B); break;
    case SPU_rotqmbyi:DST = spu_rotqmbyi(A,I); break;
    /* select / shuffle / masks / bitcount (RRR: dest=rc, srcs=ra,rb,rt) */
    case SPU_selb:  DSTC = spu_selb(A,B,T); break;
    case SPU_shufb: DSTC = spu_shufb(A,B,T); break;
    case SPU_fsm:  DST = spu_fsm(A); break;
    case SPU_fsmb: DST = spu_fsmb(A); break;
    case SPU_fsmh: DST = spu_fsmh(A); break;
    case SPU_gb:   DST = spu_gb(A); break;
    case SPU_gbb:  DST = spu_gbb(A); break;
    case SPU_gbh:  DST = spu_gbh(A); break;
    case SPU_clz:  DST = spu_clz(A); break;
    case SPU_cntb: DST = spu_cntb(A); break;
    /* gen-controls for insertion */
    case SPU_cbd: DST = spu_cbd(A,I); break;
    case SPU_chd: DST = spu_chd(A,I); break;
    case SPU_cwd: DST = spu_cwd(A,I); break;
    case SPU_cdd: DST = spu_cdd(A,I); break;
    case SPU_cbx: DST = spu_cbx(A,B); break;
    case SPU_chx: DST = spu_chx(A,B); break;
    case SPU_cwx: DST = spu_cwx(A,B); break;
    case SPU_cdx: DST = spu_cdx(A,B); break;
    /* multiply */
    case SPU_mpy:  DST = spu_mpy(A,B); break;
    case SPU_mpyu: DST = spu_mpyu(A,B); break;
    case SPU_mpyh: DST = spu_mpyh(A,B); break;
    case SPU_mpya: DSTC = spu_mpya(A,B,T); break;
    /* sign-extend */
    case SPU_xsbh: DST = spu_xsbh(A); break;
    case SPU_xshw: DST = spu_xshw(A); break;
    case SPU_xswd: DST = spu_xswd(A); break;
    /* float */
    case SPU_fa: DST = spu_fa(A,B); break;
    case SPU_fs: DST = spu_fs(A,B); break;
    case SPU_fm: DST = spu_fm(A,B); break;
    case SPU_fcgt: DST = spu_fcgt(A,B); break;
    case SPU_fceq: DST = spu_fceq(A,B); break;
    case SPU_cflts: DST = spu_cflts(A,I); break;
    case SPU_cfltu: DST = spu_cfltu(A,I); break;
    case SPU_csflt: DST = spu_csflt(A,I); break;
    case SPU_cuflt: DST = spu_cuflt(A,I); break;
    case SPU_frest:  DST = spu_frest(A); break;
    case SPU_frsqest:DST = spu_frsqest(A); break;
    case SPU_fi:     DST = spu_fi(A,B); break;
    case SPU_fcmgt:  DST = spu_fcmgt(A,B); break;
    case SPU_fcmeq:  DST = spu_fcmeq(A,B); break;
    case SPU_fesd:   DST = spu_fesd(A); break;
    case SPU_frds:   DST = spu_frds(A); break;
    case SPU_fma:  DSTC = spu_fma(A,B,T); break;    /* RRR: dest=rc */
    case SPU_fms:  DSTC = spu_fms(A,B,T); break;
    case SPU_fnms: DSTC = spu_fnms(A,B,T); break;
    /* immediate byte/halfword compares */
    case SPU_ceqbi: DST = spu_ceqbi(A,I); break;
    case SPU_cgtbi: DST = spu_cgtbi(A,I); break;
    case SPU_clgtbi:DST = spu_clgtbi(A,I); break;
    case SPU_ceqhi: DST = spu_ceqhi(A,I); break;
    case SPU_cgthi: DST = spu_cgthi(A,I); break;
    case SPU_clgthi:DST = spu_clgthi(A,I); break;
    /* integer / byte extras */
    case SPU_bg:    DST = spu_bg(A,B); break;
    case SPU_bgx:   DST = spu_bgx(A,B,DST); break;   /* RR: rt is accumulator + dest */
    case SPU_absdb: DST = spu_absdb(A,B); break;
    case SPU_avgb:  DST = spu_avgb(A,B); break;
    case SPU_sumb:  DST = spu_sumb(A,B); break;
    case SPU_mpys:  DST = spu_mpys(A,B); break;
    case SPU_mpyhh: DST = spu_mpyhh(A,B); break;
    case SPU_mpyhhu:DST = spu_mpyhhu(A,B); break;
    case SPU_mpyhha: DST = spu_mpyhha(A,B,DST); break;   /* RR (0x346): rt is accumulator + dest, NOT RRR */
    case SPU_mpyhhau:DST = spu_mpyhhau(A,B,DST); break;
    /* rotate / shift extras */
    case SPU_rothm:    DST = spu_rothm(A,B); break;
    case SPU_rothmi:   DST = spu_rothmi(A,I); break;
    case SPU_rotmah:   DST = spu_rothma(A,B); break;
    case SPU_rotmahi:  DST = spu_rotmahi(A,I); break;
    case SPU_rotqmbi:  DST = spu_rotqmbi(A,B); break;
    case SPU_rotqmbii: DST = spu_rotqmbii(A,(int)I); break;
    case SPU_rotqbybi: DST = spu_rotqbybi(A,B); break;
    case SPU_rotqmbybi:DST = spu_rotqmbybi(A,B); break;
    case SPU_shlqbybi: DST = spu_shlqbybi(A,B); break;
    /* channels */
    case SPU_wrch: spu_wrch(ctx, d.ch, DST); break;
    case SPU_rdch: DST = spu_rdch(ctx, d.ch); break;
    case SPU_rchcnt: DST = spu_pref_u32(spu_rchcnt(ctx, d.ch)); break;   /* count in the preferred word, rest zero (as the lifter) */
    /* hints / no-ops */
    case SPU_nop: case SPU_lnop: case SPU_sync: case SPU_dsync:
    case SPU_hbr: case SPU_hbra: case SPU_hbrr: case SPU_mtspr:
    case SPU_fscrwr: break;
    case SPU_mfspr: DST = spu_mfspr(A); break;      /* SPRs read as zero */
    case SPU_fscrrd: DST = spu_fscrrd(A); break;    /* FPSCR reads as zero */
    /* double precision */
    case SPU_dfa:  DST = spu_dfa(A,B); break;
    case SPU_dfs:  DST = spu_dfs(A,B); break;
    case SPU_dfm:  DST = spu_dfm(A,B); break;
    case SPU_dfma: DST = spu_dfma(A,B,DST); break;   /* RR: rt is accumulator + dest */
    case SPU_dfms: DST = spu_dfms(A,B,DST); break;
    case SPU_dfnms:DST = spu_dfnms(A,B,DST); break;
    case SPU_dfnma:DST = spu_dfnma(A,B,DST); break;
    case SPU_dfceq:  DST = spu_dfceq(A,B); break;
    case SPU_dfcgt:  DST = spu_dfcgt(A,B); break;
    case SPU_dfcmeq: DST = spu_dfcmeq(A,B); break;
    case SPU_dfcmgt: DST = spu_dfcmgt(A,B); break;
    case SPU_dftsv:  DST = spu_dftsv(A, (int32_t)(d.rb & 0x7F)); break;   /* I7 sits in the rb field */
    /* control flow */
    case SPU_br: case SPU_bra: next = d.tgt; break;
    case SPU_brsl: case SPU_brasl: DST = spu_link((pc + 4) & 0x3FFFC); next = d.tgt; call_link = (pc + 4) & 0x3FFFC;
        { extern void spu_trace_call(uint32_t,uint32_t); spu_trace_call(pc, d.tgt); } break;
    case SPU_brz:  if (PREF(DST) == 0) next = d.tgt; break;
    case SPU_brnz: if (PREF(DST) != 0) next = d.tgt; break;
    case SPU_brhz: if ((PREF(DST) & 0xFFFF) == 0) next = d.tgt; break;
    case SPU_brhnz:if ((PREF(DST) & 0xFFFF) != 0) next = d.tgt; break;
    /* T-0001 (2026-10-08): the bi/iret interrupt-enable/disable bits (E =
     * 0x40000 -> int_enable=1, D = 0x80000 -> int_enable=0, effective when the
     * branch is taken) were decoded ONLY by the lifter; the interpreter dropped
     * them, distorting every interrupt window that executes interpreted.
     * Mirror spu_lifter.py:942-943. */
    #define SPU_IED_BITS() do { \
        if (insn & 0x40000u) ctx->int_enable = 1; \
        else if (insn & 0x80000u) ctx->int_enable = 0; \
    } while (0)
    case SPU_bi:   next = PREF(A) & 0x3FFFC; SPU_IED_BITS(); break;
    case SPU_bisl: { uint32_t tg = PREF(A) & 0x3FFFC;   /* read ra BEFORE the link write: rt may alias ra */
        DST = spu_link((pc + 4) & 0x3FFFC); next = tg; call_link = (pc + 4) & 0x3FFFC; break; }
    case SPU_bisled: { uint32_t tg = PREF(A) & 0x3FFFC;
        DST = spu_link((pc + 4) & 0x3FFFC);
        if ((spu_ev_get(ctx) & ctx->event_mask) != 0) {
            next = tg; call_link = (pc + 4) & 0x3FFFC;
            SPU_IED_BITS(); }   /* E/D effective when taken, like the lift */
        break; }
    case SPU_iret: next = ctx->srr0 & 0x3FFFC;
        /* T-0001 (2026-10-08, CONFIRMED root cause): the register-file
         * save/restore contract was enforced ONLY on the lifted dispatch
         * path (spu_indirect_branch -> spu_irq_regs_maybe_restore). The
         * WWS interrupt handler's iret frequently executes HERE, in the
         * interpreter, where irq_saved was never checked -- the restore
         * was silently missed and the stale 128-GPR snapshot stayed
         * armed in the persistent LLE thread, later false-firing
         * (pc == resume && int_enable) on an unrelated dispatch and
         * memcpy'ing interrupt-time registers over live job parameters.
         * boot22's tripwires caught: 8 missed restores, 8 nested takes
         * (each losing the outer snapshot), and 1 stale restore firing
         * 145,874 steps after its take.
         *
         * Hardware contract: the handler preserves every register; state
         * it publishes lives in LS/channels. On a genuine return to the
         * take point, restore the interrupted register file. */
        SPU_IED_BITS();                              /* E/D bits, like the lift */
        if (ctx->irq_saved) {
            if (next == (ctx->irq_resume_pc & SPU_LS_MASK)) {
                memcpy(ctx->gpr, ctx->irq_gpr, sizeof ctx->irq_gpr);
                ctx->irq_saved = 0;
            } else {
                static int _n = 0;
                if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8)
                    fprintf(stderr, "[t0001-irq] INTERP IRET to %05X but save "
                            "resume=%05X -- mismatch, snapshot stays armed\n",
                            next, ctx->irq_resume_pc & SPU_LS_MASK);
            }
        }
        break;
    case SPU_biz:  if (PREF(DST) == 0) next = PREF(A) & 0x3FFFC; break;
    case SPU_binz: if (PREF(DST) != 0) next = PREF(A) & 0x3FFFC; break;
    case SPU_bihz: if ((PREF(DST) & 0xFFFF) == 0) next = PREF(A) & 0x3FFFC; break;
    case SPU_bihnz:if ((PREF(DST) & 0xFFFF) != 0) next = PREF(A) & 0x3FFFC; break;
    case SPU_stop: case SPU_stopd:
        ctx->pc = next; ctx->stop_code = (d.op == SPU_stopd) ? 0x3FFFu : (uint32_t)I;   /* stopd carries no code field: RPCS3 reports 0x3FFF */
        ctx->status = SPU_STATUS_STOPPED_BY_STOP; return 1;
    /* conditional halts (assertions): stop the SPU when the condition holds,
     * else continue. The preferred word of ra is compared. */
    case SPU_heq: case SPU_heqi: case SPU_hgt: case SPU_hgti:
    case SPU_hlgt: case SPU_hlgti: {
        uint32_t a = PREF(A), rhs = (d.op==SPU_heq||d.op==SPU_hgt||d.op==SPU_hlgt) ? PREF(B) : (uint32_t)I;
        int hit = (d.op==SPU_heq||d.op==SPU_heqi)   ? (a == rhs)
                : (d.op==SPU_hgt||d.op==SPU_hgti)   ? ((int32_t)a > (int32_t)rhs)
                :                                     (a > rhs);      /* hlgt/hlgti */
        if (hit) { extern void spu_trace_dump(uint32_t); spu_trace_dump(pc);
                   ctx->pc = pc; ctx->stop_code = 0x1000; ctx->status = SPU_STATUS_STOPPED_BY_HALT; return 1; }
        break; }
    default:
        fprintf(stderr, "[spu_interp] unimplemented op '%s' (0x%08X) at LS 0x%05X\n",
                d.op < SPU_OP_COUNT ? spu_op_name[d.op] : "?", insn, pc);
        spu_ls_watch_dump("unimplemented-op");
        /* T-0001: the recurring title-window faults die here (executing
         * data after a smashed control chain). Give this path the same
         * full LS + GPR state dump the branch-to-0 handler has, so every
         * fault variant yields a fresh 256 KB dump for offline analysis.
         * One-shot budget; path from YDKJ_SPU_LSDUMP else recomp_spu_ls.bin. */
        { static int _dumped = 0;
          if (__sync_fetch_and_add(&_dumped, 1) == 0) {
            const char* dp = getenv("YDKJ_SPU_LSDUMP");
            if (!dp || !*dp) dp = "recomp_spu_ls.bin";
            FILE* lf = fopen(dp, "wb");
            if (lf) { fwrite(ctx->ls, 1, SPU_LS_SIZE, lf); fclose(lf);
                      fprintf(stderr, "[SPU] dumped 256KB LS -> %s\n", dp); }
            fprintf(stderr, "[SPU] image_id=%d pc=0x%05X  GPR dump (r0..r127):\n",
                    ctx->image_id, ctx->pc & SPU_LS_MASK);
            for (int g = 0; g < 128; g++) {
                fprintf(stderr, " r%-3d=%08X %08X %08X %08X", g,
                        ctx->gpr[g]._u32[0], ctx->gpr[g]._u32[1],
                        ctx->gpr[g]._u32[2], ctx->gpr[g]._u32[3]);
                if ((g & 1) == 1) fprintf(stderr, "\n");
            }
            fprintf(stderr, "\n"); fflush(stderr);
          } }
        ctx->pc = pc; ctx->status = SPU_STATUS_STOPPED_BY_HALT; return 1;
    }
    /* Interpreted code CALLING lifted code: run the callee as a subroutine and
     * carry on at the link address, as the SPU does. Without this the run loop
     * saw a lifted pc, "rejoined the fast path" and returned to whatever lifted
     * code had started the interpreter -- which then continued as if ITS call
     * had returned, with the interpreted function's frame still on the stack
     * and its work half done. inFamous's SPURS jobs call code they load at
     * runtime (LS 0x21xxx/0x22xxx), which calls back into the job's lifted
     * functions; the stale frames ended with a saved link register read from
     * the job's output records and a kernel thread branching into data. */
    /* The call is made exactly as lifted code makes one (the lifter's bisl):
     * host_depth bracket, dispatch, then drain until the pc comes back to the
     * link. A callee that leaves some other way -- a context switch reloading
     * r0 -- unwinds through spu_drain_call's restart, as it would from a
     * lifted caller. */
    if (call_link && ctx->image_id >= 0 && spu_lifted_lookup(ctx, next)) {
        int32_t si = (int32_t)ctx->image_id;
        { extern void spu_xfer_log_call(const spu_context*, uint32_t, uint32_t); spu_xfer_log_call(ctx, pc, next); }
        ctx->pc = next;
        ctx->host_depth++;
        spu_indirect_branch(ctx);
        spu_drain_call(ctx, call_link);
        ctx->host_depth--;
        spu_img_restore(ctx, si);
        if (ctx->status & (SPU_STATUS_STOPPED_BY_STOP | SPU_STATUS_STOPPED_BY_HALT)) return 1;
        next = ctx->pc & 0x3FFFC;
    }
    ctx->pc = next;
    return 0;
}

SPU_THREAD_LOCAL uint32_t g_spu_interp_last_pc = 0;   /* per run, read back on the running thread */
/* Exit status of the last spu_run_interp_job on this host thread (stop 0x102). */
SPU_THREAD_LOCAL int     g_spu_interp_exit_valid  = 0;
SPU_THREAD_LOCAL int32_t g_spu_interp_exit_status = 0;
/* ...and of a sys_spu_thread_group_exit (stop 0x101): the group's status. */
SPU_THREAD_LOCAL int     g_spu_interp_group_exit_valid  = 0;
SPU_THREAD_LOCAL int32_t g_spu_interp_group_exit_status = 0;
SPU_THREAD_LOCAL uint64_t g_spu_interp_steps   = 0;

/* Call-trace ring buffer for diagnosing SPU asserts (env SPU_CALLTRACE). */
#define SPU_TRACE_N 32
static struct { uint32_t from, to; } s_trace[SPU_TRACE_N];
static unsigned s_trace_i = 0;
void spu_trace_call(uint32_t from, uint32_t to) {
    s_trace[s_trace_i % SPU_TRACE_N].from = from;
    s_trace[s_trace_i % SPU_TRACE_N].to   = to;
    s_trace_i++;
}
void spu_trace_dump(uint32_t at) {
    if (!getenv("SPU_CALLTRACE")) return;
    fprintf(stderr, "[SPU-TRACE] halt at LS 0x%05X; last %d calls (from->to):\n", at, SPU_TRACE_N);
    unsigned n = s_trace_i < SPU_TRACE_N ? s_trace_i : SPU_TRACE_N;
    for (unsigned k = 0; k < n; k++) {
        unsigned idx = (s_trace_i - n + k) % SPU_TRACE_N;
        fprintf(stderr, "   0x%05X -> 0x%05X\n", s_trace[idx].from, s_trace[idx].to);
    }
}

spu_context* volatile g_spu_oracle_trace_ctx = 0;
long g_spu_oracle_trace_left = 0;

/* SPU_INTERP_XFER_LOG=N: the first N hand-offs from interpreted code to
 * lifted code (a call, or the rejoin when the run reaches a lifted pc), with
 * the image whose lift was chosen and the word local store actually holds
 * there -- to tell "the lift owns this code" from "a stale registration for
 * some other image's code shadows what was DMA'd in since". */
static void spu_xfer_log(const spu_context* ctx, const char* kind, uint32_t from, uint32_t to)
{
    static _Atomic long s_left = -1;
    if (s_left < 0) { const char* e = getenv("SPU_INTERP_XFER_LOG"); s_left = e ? atol(e) : 0; }
    if (s_left <= 0 || s_left-- <= 0) return;
    int owner = ctx->image_id, slot_hit = -1;
    for (unsigned k = 0; k < 4; ++k)
        if (ctx->resident_code[k].image_id && to - ctx->resident_code[k].lsa < ctx->resident_code[k].size) {
            owner = ctx->resident_code[k].image_id; slot_hit = (int)k; break; }
    uint32_t w = ((uint32_t)ctx->ls[to] << 24) | ((uint32_t)ctx->ls[to+1] << 16) |
                 ((uint32_t)ctx->ls[to+2] << 8) | ctx->ls[to+3];
    /* A resident slot's code came from source_ea: compare what the lift was
     * made from with what local store holds now. */
    const char* st = "";
    uint32_t sw = 0;
    if (slot_hit >= 0 && ctx->resident_code[slot_hit].source_ea) {
        extern uint8_t* vm_base;
        uint32_t ea = ctx->resident_code[slot_hit].source_ea + (to - ctx->resident_code[slot_hit].lsa);
        sw = ((uint32_t)vm_base[ea] << 24) | ((uint32_t)vm_base[ea+1] << 16) |
             ((uint32_t)vm_base[ea+2] << 8) | vm_base[ea+3];
        st = sw == w ? " match" : " STALE";
    }
    char line[220];
    snprintf(line, sizeof line, "[interp-xfer] %s 0x%05X -> 0x%05X img=%d owner=%d slot=%d%s ls=%08X src=%08X%s r1=0x%05X\n",
             kind, from, to, ctx->image_id, owner, slot_hit,
             slot_hit >= 0 ? "" : " (base image)", w, sw, st, ctx->gpr[1]._u32[0]);
    fputs(line, stderr);
}

void spu_xfer_log_call(const spu_context* ctx, uint32_t from, uint32_t to) { spu_xfer_log(ctx, "call", from, to); }

uint32_t spu_interp_run(spu_context* ctx, uint32_t start_lsa) {
    return spu_interp_run_until(ctx, start_lsa, 0);
}

uint32_t spu_interp_run_until(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa) {
    ctx->pc = start_lsa & 0x3FFFC;
    ctx->status = SPU_STATUS_RUNNING;
    uint64_t steps = 0;
    /* YDKJ_SPU_TRACE=N: log the last N PCs into a ring buffer and dump them when the
     * interp halts -- shows the path to a branch-to-0 (the cri task/policy wall). */
    static _Atomic int _tr=-1; if(_tr<0){const char*e=getenv("YDKJ_SPU_TRACE");_tr=e?atoi(e):0;}
    static _Atomic uint64_t _cap=0; { static _Atomic int _ci=0; if(!_ci){const char*e=getenv("SPU_STEPCAP"); _cap=e?strtoull(e,0,0):0; _ci=1;} }
    /* YDKJ_CRI_GATE1TRACE=N: cri decode task busy-spins in the validator func_00026E80
     * (LS 0x26E80..0x26F14), a straight-line leaf that returns r3 = 0 / 0x8041090F /
     * 0x80410909. Hand-decoding its selb/fsm/gb/ceqh lanes proved unreliable, so log the
     * ACTUAL runtime result + inputs the first N times it returns. Fires on range-exit
     * (pc left the fn), when r3 and the leaf's non-restored intermediates are still live. */
    static _Atomic int _g1=-1; if(_g1<0){const char*e=getenv("YDKJ_CRI_GATE1TRACE");_g1=e?atoi(e):0;}
    static int _g1in=0;
    uint32_t ring[64]; int rc=0, rn=0;
    for (;;) {
        /* Match the lifted path: only synthetic HLE tasks may bypass the
         * resident policy. Real tasksets also store 0xA70 in their API table.
         * Preserve the legacy CRI return convention while invoking the HLE. */
        if (ctx->pc == 0xA70u) {
            uint32_t sc = ((uint32_t)ctx->ls[0x27C4]<<24)|((uint32_t)ctx->ls[0x27C5]<<16)
                        | ((uint32_t)ctx->ls[0x27C6]<<8) | ctx->ls[0x27C7];
            if (ctx->image_id == 22 || (ctx->policy_mode && sc == 0xA70u)) {
                extern void spu_spurs_taskset_syscall(spu_context*);
                int _save = ctx->image_id; ctx->image_id = 22;
                spu_spurs_taskset_syscall(ctx);
                ctx->image_id = _save;
                ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;   /* resume at link r0 */
                continue;
            }
        }
        /* Rejoin the compiled fast path only for images that HAVE lifted
         * functions (image_id >= 0). image_id < 0 = pure interpretation: an
         * un-lifted image (e.g. a title's raw SPU jobs) must never rejoin
         * another image's functions that happen to share an LS address. */
        /* ponytail: pc-only match; a recursive call through the same site would stop early -- compare r1 too if one shows up. */
        if (ctx->image_id >= 0 && ((stop_lsa && ctx->pc == stop_lsa) || spu_lifted_lookup(ctx, ctx->pc))) {
            { static _Atomic int s_rj = -1; if (s_rj < 0) { const char* e = getenv("SPU_REJOIN_DBG"); s_rj = e ? atoi(e) : 0; }
              if (s_rj > 0 && steps > 1000) { s_rj--; fprintf(stderr, "[rejoin] img=%d pc=0x%05X after %llu steps (from 0x%05X)\n",
                  ctx->image_id, ctx->pc, (unsigned long long)steps, start_lsa & 0x3FFFC); } }
            spu_xfer_log(ctx, "rejoin", g_spu_interp_last_pc, ctx->pc);
            g_spu_interp_steps = steps; g_spu_interp_last_pc = ctx->pc; return ctx->pc; }  /* rejoin fast path */
        g_spu_interp_last_pc = ctx->pc;
        if (_g1>0) {
            int inr = (ctx->pc >= 0x26E80u && ctx->pc < 0x26F14u);
            if (inr) { _g1in = 1; }
            else if (_g1in) {
                _g1in = 0;
                #define GLB(o) (((uint32_t)ctx->ls[(o)]<<24)|((uint32_t)ctx->ls[(o)+1]<<16)|((uint32_t)ctx->ls[(o)+2]<<8)|ctx->ls[(o)+3])
                fprintf(stderr,"[gate1] func_00026E80 ret r3=%08X | LS2FB0=%08X LS2FB4=%08X LS2FB8=%08X | LS1E0=%08X LS1E4=%08X LS1E8=%08X | r1=%05X r29=%08X r32=%08X r16=%08X r22=%08X r8=%08X r6=%08X\n",
                    ctx->gpr[3]._u32[0], GLB(0x2FB0),GLB(0x2FB4),GLB(0x2FB8), GLB(0x1E0),GLB(0x1E4),GLB(0x1E8),
                    ctx->gpr[1]._u32[0], ctx->gpr[29]._u32[0], ctx->gpr[32]._u32[0],
                    ctx->gpr[16]._u32[0], ctx->gpr[22]._u32[0], ctx->gpr[8]._u32[0], ctx->gpr[6]._u32[0]);
                #undef GLB
                fflush(stderr); _g1--;
            }
        }
        if (_tr>0) { ring[rc&63]=ctx->pc; rc++; if(rn<64)rn++; }
        /* SPU_TRACE_INTERP=1: emit the same per-instruction PC trace a --trace lift
         * does (spu_trace_pc), so an interpreted run diffs against a lifted one. */
        { static _Atomic int s_ti = -1; if (s_ti < 0) s_ti = getenv("SPU_TRACE_INTERP") ? 1 : 0;
          if (s_ti) { extern void spu_trace_pc(spu_context*, uint32_t); spu_trace_pc(ctx, ctx->pc); } }
        steps++;
        /* T-0001 (2026-10-08): ctx->steps is the cross-path instruction clock
         * (the drain loop bumps it once per lifted trampoline). The
         * interpreter kept only this LOCAL counter, so ctx->steps froze
         * during interpreted runs -- breaking the irq staleness metric and
         * any timing analysis. Advance it here per interpreted instruction
         * (closer to true rate than the trampoline approximation). */
        ctx->steps++;
        /* SPU_STEPCAP=N: a task that never halts (infinite work/wait loop) never
         * dumps its ring. Force a one-shot dump after N steps to see where it loops. */
        if (_tr>0 && _cap && steps == _cap) {
            fprintf(stderr,"[spu-trace] STEPCAP pc=0x%05X after %llu steps; last %d PCs:",
                    ctx->pc, (unsigned long long)steps, rn);
            for(int k=rn;k>0;k--) fprintf(stderr," %05X", ring[(rc-k)&63]);
            fprintf(stderr,"\n");
            /* dump the LS 0x2700 SpursTasksetContext the poll reads -- compare to the
             * working RPCS3 SPU0 dump (RUNNING/READY/ENABLED=0x80000000, SIGNALLED=0). */
            #define LB(o) (((uint32_t)ctx->ls[(o)]<<24)|((uint32_t)ctx->ls[(o)+1]<<16)|((uint32_t)ctx->ls[(o)+2]<<8)|ctx->ls[(o)+3])
            fprintf(stderr,"[spu-trace] LS ctx: RUN=%08X READY=%08X PEND=%08X ENA=%08X SIG=%08X WAIT=%08X | 2790=%08X 2794=%08X 27D8=%08X 2FB0=%08X\n",
                LB(0x2700),LB(0x2710),LB(0x2720),LB(0x2730),LB(0x2740),LB(0x2750),LB(0x2790),LB(0x2794),LB(0x27D8),LB(0x2FB0));
            #undef LB
            fflush(stderr); _tr--; g_spu_interp_steps=steps; return 0x2000u;
        }
        /* Oracle trace (armed by SPU_LS_DUMP_LIST + SPU_TRACE_N, see spu_dma.h): pc, opcode and
         * the register named by the rt field after the step -- same format as the RPCS3 oracle's
         * "[oracle] T" lines, so the two traces diff directly. */
        extern spu_context* volatile g_spu_oracle_trace_ctx; extern long g_spu_oracle_trace_left;
        const uint32_t _tpc = ctx->pc;
        const uint32_t _top = ((uint32_t)ctx->ls[_tpc] << 24) | ((uint32_t)ctx->ls[_tpc+1] << 16) |
                              ((uint32_t)ctx->ls[_tpc+2] << 8) | ctx->ls[_tpc+3];
        /* SPU_INTERP_HIST=1: which (image, 4 KB LS page) the interpreter spends
         * its steps in -- the candidates for ahead-of-time lifting. */
        { static _Atomic int on = -1; if (on < 0) on = getenv("SPU_INTERP_HIST") ? 1 : 0;
          if (on) {
              static _Atomic unsigned long long hist[64][64]; static _Atomic unsigned long long tot;
              const int img = ctx->image_id < 0 ? 63 : (ctx->image_id & 63);
              hist[img][(_tpc >> 12) & 63]++;
              if ((++tot % 200000000ull) == 0) {
                  fprintf(stderr, "[spu-hist] after %llu interpreted steps:\n", (unsigned long long)tot);
                  for (int i = 0; i < 64; i++) for (int j = 0; j < 64; j++)
                      if (hist[i][j] > tot / 50) fprintf(stderr, "[spu-hist]   img %d page 0x%05X: %.1f%%\n",
                                                        i == 63 ? -1 : i, j << 12, 100.0 * hist[i][j] / tot);
              } } }
        if (__atomic_load_n(&ctx->stop_request, __ATOMIC_RELAXED)) {   /* group terminated */
            ctx->status = SPU_STATUS_STOPPED_BY_HALT; ctx->stop_code = 0;
            g_spu_interp_steps = steps;
            return 0;
        }
        if (ctx->interp_step_budget && steps >= ctx->interp_step_budget) {
            /* test hook: budget exhausted while still running (T-0001) */
            g_spu_interp_steps = steps;
            return 0xFFFFFFFFu;
        }
        const int _st = spu_step(ctx);
        if (g_spu_oracle_trace_ctx == ctx && _tpc >= 0x3780 && g_spu_oracle_trace_left-- > 0) {
            const u128* r = &ctx->gpr[_top & 0x7F];
            fprintf(stderr, "[oracle] T %05x %08x r%u=%08x %08x %08x %08x\n", _tpc, _top, _top & 0x7F,
                    r->_u32[0], r->_u32[1], r->_u32[2], r->_u32[3]);
        }
        if (_st) {
            /* stop 0x110 = SYS_SPU_THREAD_STOP_RECEIVE_EVENT. The worker writes
             * the SPU queue number to its out-mailbox, stops, and lv2 replies
             * with {CELL_OK, data1, data2, data3} in the in-mailbox -- which is
             * how these sim SPUs receive each frame's work-descriptor EA.
             * Treating the stop as "job finished" let them resume with nothing
             * in that structure and DMA their results to address 0.
             * Service it and keep running; with no event pending, fall through
             * to the old behaviour so nothing waits forever. */
            if (ctx->stop_code == 0x110 && g_spu_pending_evt_valid) {
                spu_channel_read(&ctx->ch_out_mbox);      /* the SPU queue key */
                ctx->rcv_evt[0] = 0;                      /* CELL_OK */
                ctx->rcv_evt[1] = g_spu_pending_evt[0];
                ctx->rcv_evt[2] = g_spu_pending_evt[1];
                ctx->rcv_evt[3] = g_spu_pending_evt[2];
                ctx->rcv_evt_n = 4; ctx->rcv_evt_i = 0;
                g_spu_pending_evt_valid = 0;
                ctx->status = SPU_STATUS_RUNNING;
                continue;
            }
            /* lv2 stop-and-signal syscalls (yield, receive_event from a bound
             * SPU queue, tryreceive_event): the same service lifted threads use
             * (spu_stop). The pc is already past the stop. */
            { extern int (*g_spu_lv2_stop_hook)(spu_context*);
              if (ctx->status == SPU_STATUS_STOPPED_BY_STOP && g_spu_lv2_stop_hook &&
                  g_spu_lv2_stop_hook(ctx)) {
                  ctx->status = SPU_STATUS_RUNNING;
                  continue;
              } }
            g_spu_interp_steps = steps;
            if (_tr>0) { fprintf(stderr,"[spu-trace] halt stop=0x%X pc=0x%05X after %llu steps; last %d PCs:",
                    ctx->stop_code, ctx->pc, (unsigned long long)steps, rn);
                for(int k=rn;k>0;k--) fprintf(stderr," %05X", ring[(rc-k)&63]);
                fprintf(stderr,"\n"); fflush(stderr); _tr--; }
            return ctx->stop_code; }
    }
}

void spu_dispatch(spu_context* ctx, uint32_t target) {
    for (;;) {
        target &= 0x3FFFC;
        spu_lifted_fn fn = spu_lifted_lookup(ctx, target);
        if (fn) { ctx->pc = target; fn(ctx); return; }
        uint32_t next = spu_interp_run(ctx, target);
        if (next == 0xFFFFFFFFu) return;   /* interp_step_budget exhausted (test hook) */
        if (ctx->status & (SPU_STATUS_STOPPED_BY_STOP | SPU_STATUS_STOPPED_BY_HALT))
            return;
        if ((next & 0x3FFFC) == target) return;   /* no-progress guard */
        target = next;
    }
}

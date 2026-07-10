/* Memory-op KATs: update-form addressing, byte-reversed load/store, sign
 * extension on load, unaligned access, lwarx/stwcx, lmw/stmw.
 * These are hand-written (not generated): each needs its own buffer setup.
 */
#include "torture.h"

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

static u64 t_d2b(double d) { union { u64 u; double d; } v; v.d = d; return v.u; }

/* One shared 16-byte-aligned playground. */
static union {
    u8  b[128];
    u64 d[16];
} g_buf __attribute__((aligned(16)));

static void fill_pattern(void)
{
    int i;
    for (i = 0; i < 128; i++)
        g_buf.b[i] = (u8)(0xA0 + i);
}

/* ---- update-form loads: value AND post-increment EA ------------------- */

static void NOINLINE mem_update_forms(void)
{
    t_section("mem_update");
    fill_pattern();
    /* lwzu %0,4(%1): loads from base+4, base updated to base+4 */
    {
        u64 got; u8* base = &g_buf.b[0];
        __asm__ __volatile__("lwzu %0,4(%1)" : "=&r"(got), "+b"(base));
        t_kat("lwzu_val", 0, got, 0xA4A5A6A7ULL);
        t_kat("lwzu_ea", 0, (u64)(base - &g_buf.b[0]), 4);
    }
    {
        u64 got; u8* base = &g_buf.b[8];
        __asm__ __volatile__("lbzu %0,1(%1)" : "=&r"(got), "+b"(base));
        t_kat("lbzu_val", 0, got, 0xA9ULL);
        t_kat("lbzu_ea", 0, (u64)(base - &g_buf.b[0]), 9);
    }
    {
        u64 got; u8* base = &g_buf.b[0];
        __asm__ __volatile__("lhzu %0,6(%1)" : "=&r"(got), "+b"(base));
        t_kat("lhzu_val", 0, got, 0xA6A7ULL);
        t_kat("lhzu_ea", 0, (u64)(base - &g_buf.b[0]), 6);
    }
    {
        u64 got; u8* base = &g_buf.b[0];
        __asm__ __volatile__("ldu %0,8(%1)" : "=&r"(got), "+b"(base));
        t_kat("ldu_val", 0, got, 0xA8A9AAABACADAEAFULL);
        t_kat("ldu_ea", 0, (u64)(base - &g_buf.b[0]), 8);
    }
    /* stwu: store at base+4, base updated */
    {
        u8* base = &g_buf.b[16];
        u64 v = 0xDEADBEEF;
        __asm__ __volatile__("stwu %1,4(%0)" : "+b"(base) : "r"(v) : "memory");
        t_kat("stwu_ea", 0, (u64)(base - &g_buf.b[0]), 20);
        t_kat("stwu_mem", 0,
              ((u64)g_buf.b[20] << 24) | ((u64)g_buf.b[21] << 16) |
              ((u64)g_buf.b[22] << 8) | g_buf.b[23], 0xDEADBEEFULL);
    }
    {
        u8* base = &g_buf.b[32];
        u64 v = 0x1122334455667788ULL;
        __asm__ __volatile__("stdu %1,8(%0)" : "+b"(base) : "r"(v) : "memory");
        t_kat("stdu_ea", 0, (u64)(base - &g_buf.b[0]), 40);
        t_kat("stdu_mem", 0, g_buf.d[5], 0x1122334455667788ULL);
    }
}

/* ---- FP update-form loads/stores --------------------------------------
 * The lifter emitted these WITHOUT the rA writeback (integer u-forms had
 * it): gcc walks float arrays with `lfsu f,4(rN)`, so every later access
 * through rN re-read element 0. wave's _XyToPolar computed sqrt(x*x+x*x)
 * and the hue-palette disc came out as a vertical band. */

static void NOINLINE mem_fp_update_forms(void)
{
    t_section("mem_fp_update");
    /* two known floats/doubles at fixed offsets */
    g_buf.b[0] = 0x3F; g_buf.b[1] = 0x80; g_buf.b[2] = 0; g_buf.b[3] = 0;   /* 1.0f  */
    g_buf.b[4] = 0x40; g_buf.b[5] = 0x40; g_buf.b[6] = 0; g_buf.b[7] = 0;   /* 3.0f  */
    g_buf.d[1] = 0x4000000000000000ULL;                                     /* 2.0   */
    g_buf.d[2] = 0xC010000000000000ULL;                                     /* -4.0  */
    {
        double got; u8* base = &g_buf.b[0];
        __asm__ __volatile__("lfsu %0,4(%1)" : "=&f"(got), "+b"(base));
        t_kat("lfsu_val", 0, t_d2b(got), t_d2b(3.0));
        t_kat("lfsu_ea", 0, (u64)(base - &g_buf.b[0]), 4);
    }
    {
        double got; u8* base = &g_buf.b[0];
        __asm__ __volatile__("lfdu %0,8(%1)" : "=&f"(got), "+b"(base));
        t_kat("lfdu_val", 0, t_d2b(got), t_d2b(2.0));
        t_kat("lfdu_ea", 0, (u64)(base - &g_buf.b[0]), 8);
    }
    {
        double got; u8* base = &g_buf.b[0]; u64 idx = 4;
        __asm__ __volatile__("lfsux %0,%1,%2" : "=&f"(got), "+b"(base) : "r"(idx));
        t_kat("lfsux_val", 0, t_d2b(got), t_d2b(3.0));
        t_kat("lfsux_ea", 0, (u64)(base - &g_buf.b[0]), 4);
    }
    {
        double got; u8* base = &g_buf.b[0]; u64 idx = 16;
        __asm__ __volatile__("lfdux %0,%1,%2" : "=&f"(got), "+b"(base) : "r"(idx));
        t_kat("lfdux_val", 0, t_d2b(got), t_d2b(-4.0));
        t_kat("lfdux_ea", 0, (u64)(base - &g_buf.b[0]), 16);
    }
    {
        u8* base = &g_buf.b[32]; double v = 5.0;
        __asm__ __volatile__("stfsu %1,4(%0)" : "+b"(base) : "f"(v) : "memory");
        t_kat("stfsu_ea", 0, (u64)(base - &g_buf.b[0]), 36);
        t_kat("stfsu_mem", 0,
              ((u64)g_buf.b[36] << 24) | ((u64)g_buf.b[37] << 16) |
              ((u64)g_buf.b[38] << 8) | g_buf.b[39], 0x40A00000ULL);
    }
    {
        u8* base = &g_buf.b[40]; double v = -6.5;
        __asm__ __volatile__("stfdu %1,8(%0)" : "+b"(base) : "f"(v) : "memory");
        t_kat("stfdu_ea", 0, (u64)(base - &g_buf.b[0]), 48);
        t_kat("stfdu_mem", 0, g_buf.d[6], 0xC01A000000000000ULL);
    }
    {
        u8* base = &g_buf.b[56]; u64 idx = 8; double v = 0.5;
        __asm__ __volatile__("stfsux %1,%0,%2" : "+b"(base) : "f"(v), "r"(idx) : "memory");
        t_kat("stfsux_ea", 0, (u64)(base - &g_buf.b[0]), 64);
        t_kat("stfsux_mem", 0,
              ((u64)g_buf.b[64] << 24) | ((u64)g_buf.b[65] << 16) |
              ((u64)g_buf.b[66] << 8) | g_buf.b[67], 0x3F000000ULL);
    }
    {
        u8* base = &g_buf.b[64]; u64 idx = 8; double v = 1.5;
        __asm__ __volatile__("stfdux %1,%0,%2" : "+b"(base) : "f"(v), "r"(idx) : "memory");
        t_kat("stfdux_ea", 0, (u64)(base - &g_buf.b[0]), 72);
        t_kat("stfdux_mem", 0, g_buf.d[9], 0x3FF8000000000000ULL);
    }
    /* indexed integer update stores (stbux/sthux/stwux/stdux lacked the
     * writeback too; ldux was missing entirely) */
    fill_pattern();
    {
        u64 got; u8* base = &g_buf.b[0]; u64 idx = 8;
        __asm__ __volatile__("ldux %0,%1,%2" : "=&r"(got), "+b"(base) : "r"(idx));
        t_kat("ldux_val", 0, got, 0xA8A9AAABACADAEAFULL);
        t_kat("ldux_ea", 0, (u64)(base - &g_buf.b[0]), 8);
    }
    {
        u8* base = &g_buf.b[16]; u64 idx = 4; u64 v = 0xCAFEF00D;
        __asm__ __volatile__("stwux %1,%0,%2" : "+b"(base) : "r"(v), "r"(idx) : "memory");
        t_kat("stwux_ea", 0, (u64)(base - &g_buf.b[0]), 20);
        t_kat("stwux_mem", 0,
              ((u64)g_buf.b[20] << 24) | ((u64)g_buf.b[21] << 16) |
              ((u64)g_buf.b[22] << 8) | g_buf.b[23], 0xCAFEF00DULL);
    }
    {
        u8* base = &g_buf.b[24]; u64 idx = 8; u64 v = 0x0123456789ABCDEFULL;
        __asm__ __volatile__("stdux %1,%0,%2" : "+b"(base) : "r"(v), "r"(idx) : "memory");
        t_kat("stdux_ea", 0, (u64)(base - &g_buf.b[0]), 32);
        t_kat("stdux_mem", 0, g_buf.d[4], 0x0123456789ABCDEFULL);
    }
    {
        u8* base = &g_buf.b[48]; u64 idx = 2; u64 v = 0xBEEF;
        __asm__ __volatile__("sthux %1,%0,%2" : "+b"(base) : "r"(v), "r"(idx) : "memory");
        t_kat("sthux_ea", 0, (u64)(base - &g_buf.b[0]), 50);
        t_kat("sthux_mem", 0, ((u64)g_buf.b[50] << 8) | g_buf.b[51], 0xBEEFULL);
    }
    {
        u8* base = &g_buf.b[56]; u64 idx = 3; u64 v = 0x5A;
        __asm__ __volatile__("stbux %1,%0,%2" : "+b"(base) : "r"(v), "r"(idx) : "memory");
        t_kat("stbux_ea", 0, (u64)(base - &g_buf.b[0]), 59);
        t_kat("stbux_mem", 0, g_buf.b[59], 0x5AULL);
    }
}

/* ---- byte-reversed load/store ----------------------------------------- */

static void NOINLINE mem_byterev(void)
{
    t_section("mem_byterev");
    fill_pattern();
    {
        u64 got; const u8* p = &g_buf.b[0];
        __asm__ __volatile__("lwbrx %0,0,%1" : "=r"(got) : "r"(p));
        t_kat("lwbrx", 0, got, 0xA3A2A1A0ULL);
    }
    {
        u64 got; const u8* p = &g_buf.b[4];
        __asm__ __volatile__("lhbrx %0,0,%1" : "=r"(got) : "r"(p));
        t_kat("lhbrx", 0, got, 0xA5A4ULL);
    }
    {
        const u8* p = &g_buf.b[64];
        u64 v = 0x12345678;
        __asm__ __volatile__("stwbrx %1,0,%0" : : "r"(p), "r"(v) : "memory");
        t_kat("stwbrx", 0,
              ((u64)g_buf.b[64] << 24) | ((u64)g_buf.b[65] << 16) |
              ((u64)g_buf.b[66] << 8) | g_buf.b[67], 0x78563412ULL);
    }
    {
        const u8* p = &g_buf.b[70];
        u64 v = 0xBEEF;
        __asm__ __volatile__("sthbrx %1,0,%0" : : "r"(p), "r"(v) : "memory");
        t_kat("sthbrx", 0, ((u64)g_buf.b[70] << 8) | g_buf.b[71], 0xEFBEULL);
    }
}

/* ---- sign-extending loads --------------------------------------------- */

static void NOINLINE mem_sign_loads(void)
{
    t_section("mem_sign");
    g_buf.b[0] = 0x80; g_buf.b[1] = 0x01;             /* i16 0x8001 */
    g_buf.b[2] = 0x7F; g_buf.b[3] = 0xFF;             /* i16 0x7FFF */
    g_buf.b[4] = 0xFF; g_buf.b[5] = 0xFF; g_buf.b[6] = 0xFF; g_buf.b[7] = 0xFE;
    {
        u64 got; const u8* p = &g_buf.b[0];
        __asm__ __volatile__("lha %0,0(%1)" : "=r"(got) : "b"(p));
        t_kat("lha_neg", 0, got, 0xFFFFFFFFFFFF8001ULL);
        __asm__ __volatile__("lha %0,2(%1)" : "=r"(got) : "b"(p));
        t_kat("lha_pos", 0, got, 0x7FFFULL);
        __asm__ __volatile__("lwa %0,4(%1)" : "=r"(got) : "b"(p));
        t_kat("lwa_neg", 0, got, 0xFFFFFFFFFFFFFFFEULL);
        __asm__ __volatile__("lhax %0,%1,%2" : "=r"(got) : "b"(p), "r"(0ULL));
        t_kat("lhax_neg", 0, got, 0xFFFFFFFFFFFF8001ULL);
        __asm__ __volatile__("lwax %0,%1,%2" : "=r"(got) : "b"(p), "r"(4ULL));
        t_kat("lwax_neg", 0, got, 0xFFFFFFFFFFFFFFFEULL);
    }
}

/* ---- unaligned accesses ------------------------------------------------ */

static void NOINLINE mem_unaligned(void)
{
    t_section("mem_unaligned");
    fill_pattern();
    {
        u64 got; const u8* p = &g_buf.b[1];           /* +1: unaligned lwz */
        __asm__ __volatile__("lwz %0,0(%1)" : "=r"(got) : "b"(p));
        t_kat("lwz_off1", 0, got, 0xA1A2A3A4ULL);
    }
    {
        u64 got; const u8* p = &g_buf.b[3];           /* +3: unaligned ld */
        __asm__ __volatile__("ld %0,0(%1)" : "=r"(got) : "b"(p));
        t_kat("ld_off3", 0, got, 0xA3A4A5A6A7A8A9AAULL);
    }
    {
        const u8* p = &g_buf.b[81];                   /* unaligned std */
        u64 v = 0x0123456789ABCDEFULL;
        __asm__ __volatile__("std %1,0(%0)" : : "b"(p), "r"(v) : "memory");
        t_kat("std_off1", 0,
              ((u64)g_buf.b[81] << 56) | ((u64)g_buf.b[82] << 48) |
              ((u64)g_buf.b[83] << 40) | ((u64)g_buf.b[84] << 32) |
              ((u64)g_buf.b[85] << 24) | ((u64)g_buf.b[86] << 16) |
              ((u64)g_buf.b[87] << 8) | g_buf.b[88], v);
    }
}

/* ---- lwarx/stwcx. success path ----------------------------------------- */

static void NOINLINE mem_atomics(void)
{
    t_section("mem_atomics");
    g_buf.d[0] = 0;
    {
        u64 old, cr; u32* p = (u32*)&g_buf.b[0];
        __asm__ __volatile__(
            "1: lwarx %0,0,%2\n\t"
            "addi %0,%0,7\n\t"
            "stwcx. %0,0,%2\n\t"
            "bne- 1b\n\t"
            "mfcr %1"
            : "=&r"(old), "=&r"(cr) : "r"(p) : "cc", "memory");
        t_kat("lwarx_stwcx_val", 0, old & 0xFFFFFFFF, 7);
        t_kat("lwarx_stwcx_mem", 0,
              ((u64)g_buf.b[0] << 24) | ((u64)g_buf.b[1] << 16) |
              ((u64)g_buf.b[2] << 8) | g_buf.b[3], 7);
    }
    {
        u64 old, cr; u64* p = &g_buf.d[1];
        *p = 0x100000000ULL;
        __asm__ __volatile__(
            "1: ldarx %0,0,%2\n\t"
            "addi %0,%0,1\n\t"
            "stdcx. %0,0,%2\n\t"
            "bne- 1b\n\t"
            "mfcr %1"
            : "=&r"(old), "=&r"(cr) : "r"(p) : "cc", "memory");
        t_kat("ldarx_stdcx", 0, g_buf.d[1], 0x100000001ULL);
    }
}

/* ---- lmw/stmw (32-bit multiword, still legal in 64-bit mode) ----------- */

static void NOINLINE mem_multiword(void)
{
    t_section("mem_multiword");
    {
        int i;
        for (i = 0; i < 8; i++) {
            g_buf.b[i * 4 + 0] = 0x10;
            g_buf.b[i * 4 + 1] = 0x20;
            g_buf.b[i * 4 + 2] = 0x30;
            g_buf.b[i * 4 + 3] = (u8)i;
        }
    }
    {
        /* pin the base OUTSIDE r24-r31: gcc 4.1 happily allocates a
         * clobbered reg for an input, and lmw's RA must not be in the
         * loaded range */
        u64 r24v, r31v;
        register const u8* p __asm__("r9") = &g_buf.b[0];
        __asm__ __volatile__(
            "lmw 24,0(%2)\n\t"
            "mr %0,24\n\t"
            "mr %1,31"
            : "=&r"(r24v), "=&r"(r31v) : "b"(p)
            : "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31");
        t_kat("lmw_first", 0, r24v, 0x10203000ULL);
        t_kat("lmw_last", 0, r31v, 0x10203007ULL);
    }
    {
        register u8* p __asm__("r9") = &g_buf.b[64];
        __asm__ __volatile__(
            "li 28,0x11\n\t"
            "li 29,0x22\n\t"
            "li 30,0x33\n\t"
            "li 31,0x44\n\t"
            "stmw 28,0(%0)"
            : : "b"(p)
            : "r28", "r29", "r30", "r31", "memory");
        t_kat("stmw", 0,
              ((u64)g_buf.b[67]) | ((u64)g_buf.b[71] << 8) |
              ((u64)g_buf.b[75] << 16) | ((u64)g_buf.b[79] << 24),
              0x44332211ULL);
    }
}

void torture_mem_run(void)
{
    mem_update_forms();
    mem_fp_update_forms();
    mem_byterev();
    mem_sign_loads();
    mem_unaligned();
    mem_atomics();
    mem_multiword();
}

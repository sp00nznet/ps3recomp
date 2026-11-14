/* clang -std=gnu17 -I runtime/spu -I include \
 * runtime/spu/tests/test_spu_numeric_semantics.c -lm -o /tmp/spu-numeric && /tmp/spu-numeric
 * Covers SPU operations used by Yakuza's geometry and audio programs. */
#include "spu_helpers.h"
#include <math.h>
#include <stdio.h>
static int passed, failed;
#define CHECK(c) do { if(c) ++passed; else { ++failed; printf("FAIL line %d: %s\n",__LINE__,#c); } } while(0)
int main(void)
{
    u128 a = spu_zero(), r;
    a._f32[0]=1.0f; a._f32[1]=-2.75f; a._f32[2]=42.5f; a._f32[3]=1024.0f;
    r=spu_cflts(a,173);
    CHECK(r._s32[0]==1 && r._s32[1]==-2 && r._s32[2]==42 && r._s32[3]==1024);
    r=spu_cfltu(a,173);
    CHECK(r._u32[0]==1 && r._u32[1]==0 && r._u32[2]==42 && r._u32[3]==1024);
    r=spu_cflts(a,172); CHECK(r._s32[0]==2 && r._s32[1]==-5);
    /* cflts/cfltu of an exponent-0 (zero/denormal) input: the significand is 0, but the console
     * still saturates on the shift amount sh = be + 23 - i8 (signed at sh >= 8, unsigned at
     * sh >= 9; sh == 8 does not saturate unsigned and a negative unsigned input stays 0).
     * Console probe, CECHC04 on FW 4.81: these exponent-0 lanes were measured at I8 0..3, 10, 15
     * and 16 (cflts) and 0..3, 15 and 16 (cfltu); the I8 15/16 rows pin the thresholds at sh 8
     * and 7, and spu_cfl_console_lanes.inc carries them as console rows too. */
    u128 q=spu_zero();
    q._u32[0]=0x00000000u;                         /* +0 */
    q._u32[1]=0x803EA729u;                         /* -denormal */
    q._u32[2]=0x80652112u;                         /* -denormal */
    for (int i8=0; i8<=3; ++i8) {
        r=spu_cflts(q,i8);                         /* sh = 20..23: measured saturating */
        CHECK(r._u32[0]==0x7FFFFFFFu && r._u32[1]==0x80000000u
              && r._u32[2]==0x80000000u && r._u32[3]==0x7FFFFFFFu);
        r=spu_cfltu(q,i8);
        CHECK(r._u32[0]==0xFFFFFFFFu && r._u32[1]==0 && r._u32[2]==0 && r._u32[3]==0xFFFFFFFFu);
    }
    r=spu_cflts(q,10);                             /* sh = 13: measured */
    CHECK(r._u32[0]==0x7FFFFFFFu && r._u32[1]==0x80000000u
          && r._u32[2]==0x80000000u && r._u32[3]==0x7FFFFFFFu);
    r=spu_cflts(q,15);                             /* sh = 8: the signed threshold */
    CHECK(r._u32[0]==0x7FFFFFFFu && r._u32[1]==0x80000000u
          && r._u32[2]==0x80000000u && r._u32[3]==0x7FFFFFFFu);
    r=spu_cfltu(q,15);                             /* sh = 8: unsigned does not saturate */
    CHECK(r._u32[0]==0 && r._u32[1]==0 && r._u32[2]==0 && r._u32[3]==0);
    r=spu_cflts(q,16);                             /* sh = 7: below both thresholds */
    CHECK(r._u32[0]==0 && r._u32[1]==0 && r._u32[2]==0 && r._u32[3]==0);
    r=spu_cfltu(q,16);
    CHECK(r._u32[0]==0 && r._u32[1]==0 && r._u32[2]==0 && r._u32[3]==0);
    /* A nonzero lane at I8 15 still converts: 1.0 * 2^158 saturates signed, and the
     * exponent-0 lanes saturate too (sh = 8 for every be at this scale). */
    a=spu_zero(); a._u32[0]=0x3F800000u; a._u32[1]=0x3F800000u;
    r=spu_cflts(a,15);
    CHECK(r._u32[0]==0x7FFFFFFFu && r._u32[1]==0x7FFFFFFFu
          && r._u32[2]==0x7FFFFFFFu && r._u32[3]==0x7FFFFFFFu);
    /* Console lanes, I8 0..16 (spu_cfl_console_lanes.inc). Each row goes into every lane in turn,
     * the other lanes holding 1.0 (modelled at every scale). A modelled row must give the
     * console's value and no stop; an unmodelled one (sh >= 264 outside the cfltu rules) must be
     * reported as that lane. */
    {
        enum { OP_S = 0, OP_U = 1, K_M = 1, K_U = 0 };
        static const struct { int uns, i8; uint32_t in, console; int modelled; } rows[] = {
#define V(op, i8, in, console, kind) { OP_##op, i8, in, console, K_##kind },
#define OP_cflts OP_S
#define OP_cfltu OP_U
#include "spu_cfl_console_lanes.inc"
#undef V
#undef OP_cflts
#undef OP_cfltu
        };
        int per_op[2] = {0, 0}, per_op_m[2] = {0, 0};
        for (size_t k = 0; k < sizeof rows / sizeof rows[0]; ++k) {
            per_op[rows[k].uns]++; per_op_m[rows[k].uns] += rows[k].modelled;
            for (int lane = 0; lane < 4; ++lane) {
                u128 v = spu_splat_u32(0x3F800000u);
                v._u32[lane] = rows[k].in;
                const int stop = spu_cfl_unmodelled_lane(v, rows[k].i8, rows[k].uns);
                if (rows[k].modelled) {
                    r = rows[k].uns ? spu_cfltu(v, rows[k].i8) : spu_cflts(v, rows[k].i8);
                    CHECK(stop == -1 && r._u32[lane] == rows[k].console);
                } else {
                    CHECK(stop == lane);
                }
            }
        }
        CHECK(per_op[0] >= 30 && per_op[1] >= 30);                 /* every op has rows */
        CHECK(per_op_m[0] >= 10 && per_op_m[1] >= 10);             /* modelled ones */
        CHECK(per_op[0] > per_op_m[0] && per_op[1] > per_op_m[1]); /* and unmodelled ones */
    }
    /* The boundaries of the rule, built from fields rather than taken from the table.
     * sh = be + 23 - I8. Words: sign | be << 23 | fraction. */
    {
#define W(neg, be, frac) (((uint32_t)(neg) << 31) | ((uint32_t)(be) << 23) | (uint32_t)(frac))
        /* End of the modelled region: sh 263 is modelled for both ops and both signs. */
        a = spu_splat_u32(W(0, 254, 0x5BAACA));                    /* I8 14: sh = 263 */
        CHECK(spu_cfl_unmodelled_lane(a, 14, 0) == -1 && spu_cfl_unmodelled_lane(a, 14, 1) == -1);
        r = spu_cflts(a, 14); CHECK(r._u32[0] == 0x7FFFFFFFu && r._u32[3] == 0x7FFFFFFFu);
        r = spu_cfltu(a, 14); CHECK(r._u32[0] == 0xFFFFFFFFu && r._u32[3] == 0xFFFFFFFFu);
        a = spu_splat_u32(W(1, 254, 0x5BAACA));
        CHECK(spu_cfl_unmodelled_lane(a, 14, 0) == -1 && spu_cfl_unmodelled_lane(a, 14, 1) == -1);
        r = spu_cflts(a, 14); CHECK(r._u32[0] == 0x80000000u);
        r = spu_cfltu(a, 14); CHECK(r._u32[0] == 0);
        a = spu_splat_u32(W(0, 240, 0x78EB45));                    /* I8 0: sh = 263 */
        CHECK(spu_cfl_unmodelled_lane(a, 0, 0) == -1 && spu_cfl_unmodelled_lane(a, 0, 1) == -1);
        /* sh 264: cflts is unmodelled for both signs; cfltu is modelled (positive 0xFFFFFFFF,
         * negative 0) -- the observed sh == 264 case. */
        for (int neg = 0; neg < 2; ++neg) {
            a = spu_splat_u32(W(neg, 255, 0x2BDE3D));              /* I8 14: sh = 264 */
            CHECK(spu_cfl_unmodelled_lane(a, 14, 0) == 0);
            CHECK(spu_cfl_unmodelled_lane(a, 14, 1) == -1);
            r = spu_cfltu(a, 14); CHECK(r._u32[1] == (neg ? 0u : 0xFFFFFFFFu));
            a = spu_splat_u32(W(neg, 254, 0x2BDE3D));              /* I8 13: sh = 264 */
            CHECK(spu_cfl_unmodelled_lane(a, 13, 0) == 0);
            CHECK(spu_cfl_unmodelled_lane(a, 13, 1) == -1);
            a = spu_splat_u32(W(neg, 241, 0x2BDE3D));              /* I8 0: sh = 264 */
            CHECK(spu_cfl_unmodelled_lane(a, 0, 0) == 0);
            CHECK(spu_cfl_unmodelled_lane(a, 0, 1) == -1);
        }
        /* sh 265 and above: cfltu stops on a positive lane, a negative one is still 0. */
        a = spu_splat_u32(W(0, 255, 0));                           /* I8 13: sh = 265 */
        CHECK(spu_cfl_unmodelled_lane(a, 13, 1) == 0);
        a = spu_splat_u32(W(1, 255, 0));
        CHECK(spu_cfl_unmodelled_lane(a, 13, 1) == -1);
        r = spu_cfltu(a, 13); CHECK(r._u32[2] == 0);
        a = spu_splat_u32(W(0, 242, 0));                           /* I8 0: sh = 265 */
        CHECK(spu_cfl_unmodelled_lane(a, 0, 1) == 0 && spu_cfl_unmodelled_lane(a, 0, 0) == 0);
        a = spu_splat_u32(W(1, 242, 0));
        CHECK(spu_cfl_unmodelled_lane(a, 0, 1) == -1 && spu_cfl_unmodelled_lane(a, 0, 0) == 0);
        /* The first unmodelled lane is the one named. */
        a = spu_zero(); a._u32[0] = 0x3F800000u; a._u32[1] = 0x3F800000u;
        a._u32[2] = W(0, 255, 0); a._u32[3] = W(1, 255, 0);
        CHECK(spu_cfl_unmodelled_lane(a, 5, 0) == 2);
        a._u32[2] = 0x3F800000u;
        CHECK(spu_cfl_unmodelled_lane(a, 5, 0) == 3);
        /* I8 >= 15 never stops: the largest sh is 263, whatever the word. */
        for (int i8 = 15; i8 < 256; ++i8) {
            a = spu_zero(); a._u32[0] = W(0, 255, 0x7FFFFF); a._u32[1] = W(1, 255, 0x7FFFFF);
            a._u32[2] = W(0, 254, 1); a._u32[3] = W(1, 0, 0);
            CHECK(spu_cfl_unmodelled_lane(a, i8, 0) == -1 && spu_cfl_unmodelled_lane(a, i8, 1) == -1);
        }
        /* The saturation thresholds on normal lanes (I8 30: sh = be - 7). The significand is
         * 0xFFFFFF, so sh 7 is 0x7FFFFF80, sh 8 would be 0xFFFFFF00 -- past the signed range
         * (saturates) but inside the unsigned one -- and sh 9 saturates both. */
        a = spu_splat_u32(W(0, 14, 0x7FFFFF)); r = spu_cflts(a, 30); CHECK(r._u32[0] == 0x7FFFFF80u);
        r = spu_cfltu(a, 30); CHECK(r._u32[0] == 0x7FFFFF80u);
        a = spu_splat_u32(W(1, 14, 0x7FFFFF)); r = spu_cflts(a, 30); CHECK(r._u32[0] == 0x80000080u);
        a = spu_splat_u32(W(0, 15, 0x7FFFFF)); r = spu_cflts(a, 30); CHECK(r._u32[0] == 0x7FFFFFFFu);
        r = spu_cfltu(a, 30); CHECK(r._u32[0] == 0xFFFFFF00u);
        a = spu_splat_u32(W(1, 15, 0x7FFFFF)); r = spu_cflts(a, 30); CHECK(r._u32[0] == 0x80000000u);
        r = spu_cfltu(a, 30); CHECK(r._u32[0] == 0);
        a = spu_splat_u32(W(0, 16, 0x7FFFFF)); r = spu_cflts(a, 30); CHECK(r._u32[0] == 0x7FFFFFFFu);
        r = spu_cfltu(a, 30); CHECK(r._u32[0] == 0xFFFFFFFFu);
        a = spu_splat_u32(W(1, 16, 0x7FFFFF)); r = spu_cflts(a, 30); CHECK(r._u32[0] == 0x80000000u);
#undef W
    }
    a=spu_splat_u32(8); r=spu_csflt(a,155); CHECK(r._f32[0]==8.0f);
    r=spu_cuflt(a,156); CHECK(r._f32[0]==16.0f);
    r=spu_csflt(a,154); CHECK(r._f32[0]==4.0f);
    a._u32[0]=0x12345678; a._u32[1]=0x80000000;
    a._u32[2]=0xffffffff; a._u32[3]=7;
    r=spu_xswd(a);
    CHECK(r._u32[0]==0xffffffff && r._u32[1]==0x80000000 && r._u32[2]==0 && r._u32[3]==7);
    a._f32[0]=1.0f; a._f32[1]=99.0f; a._f32[2]=2.0f; a._f32[3]=-99.0f;
    r=spu_fesd(a); CHECK(spu__dget(r,0)==1.0 && spu__dget(r,1)==2.0);
    r=spu_frds(r); CHECK(r._f32[0]==1.0f && r._u32[1]==0 && r._f32[2]==2.0f && r._u32[3]==0);
    /* A denormal single extends to a zero that keeps its sign (console probe, CECHC04 FW 4.81:
     * 0x80000001 -> -0.0), so only the sign of the zero distinguishes these two lanes. */
    a=spu_zero(); a._u32[0]=0x80000001u; a._u32[2]=0x007FFFFFu;
    r=spu_fesd(a); CHECK(spu__dw_get(r,0)==0x8000000000000000ull && spu__dw_get(r,1)==0);
    /* The estimate/FI pair feeds one Newton refinement in the guest code. */
    const float values[]={1.0f,2.0f,5.3311235f,-76.91874f};
    u128 one=spu_zero(); for(int i=0;i<4;++i) one._f32[i]=1.0f;
    for(int i=0;i<4;++i) a._f32[i]=values[i];
    r=spu_fi(a,spu_frest(a)); r=spu_fma(spu_fnms(a,r,one),r,r);
    for(int i=0;i<4;++i) CHECK(fabsf(r._f32[i]*values[i]-1.0f)<2e-6f);
    for(int i=0;i<4;++i) a._f32[i]=fabsf(values[i]);
    u128 half=one; for(int i=0;i<4;++i) half._f32[i]=0.5f;
    r=spu_fi(a,spu_frsqest(a));
    r=spu_fma(spu_fnms(spu_fm(a,r),r,one),spu_fm(r,half),r);
    for(int i=0;i<4;++i) CHECK(fabsf(r._f32[i]*sqrtf(a._f32[i])-1.0f)<2e-6f);
    printf("SPU numeric semantics: %d passed, %d failed\n",passed,failed);
    return failed?1:0;
}

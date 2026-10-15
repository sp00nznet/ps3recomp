/* Synthetic NV40 program: r0 = frac(WPOS * {0.25,0.25,0,0}).
 * A four-pixel color grid makes origin and half-pixel changes distinguishable
 * in an 8-bit render target, without depending on game shaders or assets. */
#ifndef FOLKLORE_WPOS_PROGRAM_H
#define FOLKLORE_WPOS_PROGRAM_H
#include "../libs/video/rsx_test_programs.h"
static inline void folklore_wpos_program(u8* p)
{
    rsx_test_fp_instr(p, RSX_TEST_FP_OPC(0x02) | RSX_TEST_FP_MASK_XYZW,
                     RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_put_word(p+8, 2u | RSX_TEST_FP_SWZ_IDENT); /* inline constant */
    rsx_test_fp_put_word(p+16, 0x3E800000u); /* .25 */
    rsx_test_fp_put_word(p+20, 0x3E800000u);
    rsx_test_fp_put_word(p+24, 0u); rsx_test_fp_put_word(p+28, 0u);
    rsx_test_fp_instr(p+32, RSX_TEST_FP_OPC(0x10) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
                     RSX_TEST_FP_SWZ_IDENT); /* FRC r0,r0 */
}
#endif

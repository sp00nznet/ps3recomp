/* Synthetic fragment program: MOV r0, WPOS.zzzz. No game data. */
#ifndef FOLKLORE_DEPTH_PROGRAM_H
#define FOLKLORE_DEPTH_PROGRAM_H
#include "../libs/video/rsx_test_programs.h"
static inline void folklore_depth_program(u8* p)
{
    rsx_test_fp_instr(p, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) |
        RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ(2,2,2,2));
}
#endif

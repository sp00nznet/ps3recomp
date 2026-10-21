#ifndef SPU_FP_CONTROL_H
#define SPU_FP_CONTROL_H
#include "spu_context.h"
#include "ps3emu/strict_boot.h"
#include <fenv.h>

static inline void spu_fp_control_fail(spu_context*ctx,uint32_t pc,const char*reason)
{
    ctx->pc=pc;
    ps3_strict_boot_guard("spu-fp-control-unimplemented",
        "pc=0x%05X lr=0x%05X spu=%u reason=%s",pc,ctx->gpr[0]._u32[0],ctx->spu_id,reason);
}
static inline u128 spu_fp_read_control(spu_context*ctx,uint32_t pc)
{
    if(ctx->fp_status_untracked&&ps3_strict_boot_latched())
        spu_fp_control_fail(ctx,pc,"arithmetic-status-not-yet-tracked");
    u128 result;spu_fpscr_read(&ctx->fpscr,result._u32);return result;
}
static inline void spu_fp_write_control(spu_context*ctx,u128 value)
{
    spu_fpscr_write(&ctx->fpscr,value._u32);
    ctx->fp_status_untracked=0;
}
static inline void spu_fp_note_arithmetic(spu_context*ctx,uint32_t pc,int double_precision)
{
    if(double_precision&&ps3_strict_boot_latched()&&
       (spu_fpscr_get_rounding(&ctx->fpscr,0)!=SPU_FPSCR_RN_NEAR||
        spu_fpscr_get_rounding(&ctx->fpscr,1)!=SPU_FPSCR_RN_NEAR||
        fegetround()!=FE_TONEAREST))
        spu_fp_control_fail(ctx,pc,"double-rounding-mode-not-yet-implemented");
    ctx->fp_status_untracked=1;
}
#endif

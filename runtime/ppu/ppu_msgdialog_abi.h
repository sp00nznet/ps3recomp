/* Include after the host's ppu_context definition. PPC float arguments are in
 * FPRs; routing this NID through the integer-only adapter loses the delay. */
#ifndef PS3RECOMP_PPU_MSGDIALOG_ABI_H
#define PS3RECOMP_PPU_MSGDIALOG_ABI_H
extern "C" int32_t cellMsgDialogClose(float delay_ms);
static inline void ppu_msgdialog_close(ppu_context* ctx)
{
    ctx->gpr[3]=(uint64_t)(int64_t)cellMsgDialogClose((float)ctx->fpr[1]);
}
#endif

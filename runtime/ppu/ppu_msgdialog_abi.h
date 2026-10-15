/* Include after the host's ppu_context definition. PPC float arguments are in
 * FPRs; routing this NID through the integer-only adapter loses the delay. */
#ifndef PS3RECOMP_PPU_MSGDIALOG_ABI_H
#define PS3RECOMP_PPU_MSGDIALOG_ABI_H
extern "C" int32_t cellMsgDialogClose(float delay_ms);
extern "C" int32_t cellMsgDialogOpenErrorCode(uint32_t error_code,
    void (*callback)(int32_t,void*),void* userdata,void* ext_param);
static inline void ppu_msgdialog_close(ppu_context* ctx)
{
    ctx->gpr[3]=(uint64_t)(int64_t)cellMsgDialogClose((float)ctx->fpr[1]);
}
static inline void ppu_msgdialog_open_error(ppu_context* ctx)
{
    const int32_t result=cellMsgDialogOpenErrorCode((uint32_t)ctx->gpr[3],
        reinterpret_cast<void (*)(int32_t,void*)>((uintptr_t)(uint32_t)ctx->gpr[4]),
        reinterpret_cast<void*>((uintptr_t)(uint32_t)ctx->gpr[5]),
        reinterpret_cast<void*>((uintptr_t)(uint32_t)ctx->gpr[6]));
    ctx->gpr[3]=(uint64_t)(int64_t)result;
}
#endif

/*
 * ps3recomp - sceNpCommerce2 HLE
 *
 * PlayStation Store commerce API (NP Commerce 2).
 * Used for in-game purchases and DLC. Safe to stub for offline play.
 */

#ifndef PS3RECOMP_SCE_NP_COMMERCE2_H
#define PS3RECOMP_SCE_NP_COMMERCE2_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED  (s32)0x80024401
#define SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT (s32)0x80024402
#define SCE_NP_COMMERCE2_ERROR_SERVER_ERROR     (s32)0x80024410

/* Context handle */
typedef u32 SceNpCommerce2Context;

/* Functions */
s32 sceNpCommerce2Init(void);
s32 sceNpCommerce2Term(void);
s32 sceNpCommerce2CreateCtx(u32 version, const void* npId,
                              SceNpCommerce2Context* ctx);
s32 sceNpCommerce2DestroyCtx(SceNpCommerce2Context ctx);
s32 sceNpCommerce2CreateSessionStart(SceNpCommerce2Context ctx);
s32 sceNpCommerce2CreateSessionFinish(SceNpCommerce2Context ctx);
s32 sceNpCommerce2GetSessionInfo(SceNpCommerce2Context ctx, void* info);
s32 sceNpCommerce2GetCategoryContentsStart(SceNpCommerce2Context ctx,
                                             const char* categoryId,
                                             u32 startPosition, u32 maxItems);
s32 sceNpCommerce2GetCategoryContentsFinish(SceNpCommerce2Context ctx);
s32 sceNpCommerce2GetCategoryContentsGetResult(SceNpCommerce2Context ctx,
                                                 void* result);
s32 sceNpCommerce2GetProductInfoStart(SceNpCommerce2Context ctx,
                                        const char* productId);
s32 sceNpCommerce2GetProductInfoFinish(SceNpCommerce2Context ctx);
s32 sceNpCommerce2GetProductInfoGetResult(SceNpCommerce2Context ctx,
                                            void* result);
s32 sceNpCommerce2AbortReq(SceNpCommerce2Context ctx);
s32 sceNpCommerce2DestroyReq(SceNpCommerce2Context ctx);
s32 sceNpCommerce2DoCheckoutFinishAsync(SceNpCommerce2Context ctx);

#ifdef __cplusplus
}
#endif
#endif

/*
 * ps3recomp - sceNpCommerce HLE
 *
 * PlayStation Store / in-game commerce: product browsing, purchasing,
 * and entitlement checking. Offline stub.
 */

#ifndef PS3RECOMP_SCE_NP_COMMERCE_H
#define PS3RECOMP_SCE_NP_COMMERCE_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED     0x80024301
#define SCE_NP_COMMERCE2_ERROR_ALREADY_INITIALIZED 0x80024302
#define SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT    0x80024303
#define SCE_NP_COMMERCE2_ERROR_NOT_CONNECTED       0x80024304
#define SCE_NP_COMMERCE2_ERROR_INSUFFICIENT        0x80024305

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef u32 SceNpCommerce2Ctx;

typedef struct SceNpCommerce2CategoryInfo {
    char categoryId[64];
    char categoryName[128];
    u32  countOfSubCategory;
    u32  countOfProduct;
} SceNpCommerce2CategoryInfo;

typedef struct SceNpCommerce2ProductInfo {
    char productId[64];
    char productName[128];
    char priceStr[32];
    u32  priceCents;
    u8   isPurchased;
    u8   reserved[3];
} SceNpCommerce2ProductInfo;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 sceNpCommerce2Init(void);
s32 sceNpCommerce2Term(void);

s32 sceNpCommerce2CreateCtx(u32 version, const char* npCommId,
                              SceNpCommerce2Ctx* ctx);
s32 sceNpCommerce2DestroyCtx(SceNpCommerce2Ctx ctx);

s32 sceNpCommerce2GetCategoryContentsStart(SceNpCommerce2Ctx ctx,
                                             const char* categoryId,
                                             u32 startPosition, u32 maxEntries);
s32 sceNpCommerce2GetCategoryInfo(SceNpCommerce2Ctx ctx,
                                    SceNpCommerce2CategoryInfo* info);
s32 sceNpCommerce2GetProductInfo(SceNpCommerce2Ctx ctx, u32 index,
                                   SceNpCommerce2ProductInfo* info);

/* Store overlay */
s32 sceNpCommerce2InitStoreRequest(SceNpCommerce2Ctx ctx);
s32 sceNpCommerce2DoCheckoutStartAsync(SceNpCommerce2Ctx ctx,
                                         const char* productId);
s32 sceNpCommerce2DoCheckoutFinishAsync(SceNpCommerce2Ctx ctx);
s32 sceNpCommerce2GetResult(SceNpCommerce2Ctx ctx, s32* result);

/* Entitlements */
s32 sceNpCommerce2DoEntitlementListStartAsync(SceNpCommerce2Ctx ctx);
s32 sceNpCommerce2DoEntitlementListFinishAsync(SceNpCommerce2Ctx ctx);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_COMMERCE_H */

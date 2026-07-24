/*
 * ps3recomp - sceNpCommerce HLE implementation
 *
 * Offline stub. Store requests return NOT_CONNECTED; games typically handle
 * this gracefully with "store unavailable" UI.
 *
 * NOTE: lifecycle + category-contents entry points (Init/Term/CreateCtx/
 * DestroyCtx/GetCategoryContentsStart/DoCheckoutFinishAsync) live in
 * sceNpCommerce2.c (the fuller module); this file keeps only the request
 * stubs that module doesn't define. Defining any of them in both files is a
 * duplicate-symbol link error in ps3recomp_runtime.lib.
 */

#include "sceNpCommerce.h"
#include <stdio.h>
#include <string.h>

s32 sceNpCommerce2GetCategoryInfo(SceNpCommerce2Ctx ctx,
                                    SceNpCommerce2CategoryInfo* info)
{
    (void)ctx; (void)info;
    return (s32)SCE_NP_COMMERCE2_ERROR_NOT_CONNECTED;
}

s32 sceNpCommerce2GetProductInfo(SceNpCommerce2Ctx ctx, u32 index,
                                   SceNpCommerce2ProductInfo* info)
{
    (void)ctx; (void)index; (void)info;
    return (s32)SCE_NP_COMMERCE2_ERROR_NOT_CONNECTED;
}

s32 sceNpCommerce2InitStoreRequest(SceNpCommerce2Ctx ctx)
{
    (void)ctx;
    return CELL_OK;
}

s32 sceNpCommerce2DoCheckoutStartAsync(SceNpCommerce2Ctx ctx,
                                         const char* productId)
{
    (void)ctx; (void)productId;
    printf("[sceNpCommerce] DoCheckoutStartAsync()\n");
    return (s32)SCE_NP_COMMERCE2_ERROR_NOT_CONNECTED;
}

s32 sceNpCommerce2GetResult(SceNpCommerce2Ctx ctx, s32* result)
{
    (void)ctx;
    if (result) *result = (s32)SCE_NP_COMMERCE2_ERROR_NOT_CONNECTED;
    return CELL_OK;
}

s32 sceNpCommerce2DoEntitlementListStartAsync(SceNpCommerce2Ctx ctx)
{
    (void)ctx;
    return (s32)SCE_NP_COMMERCE2_ERROR_NOT_CONNECTED;
}

s32 sceNpCommerce2DoEntitlementListFinishAsync(SceNpCommerce2Ctx ctx)
{
    (void)ctx;
    return CELL_OK;
}

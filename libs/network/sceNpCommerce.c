/*
 * ps3recomp - sceNpCommerce HLE implementation
 *
 * Offline stub. Init/term work, store requests return NOT_CONNECTED.
 * Games typically handle this gracefully with "store unavailable" UI.
 */

#include "sceNpCommerce.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_initialized = 0;

#define MAX_COMMERCE_CTX 4

typedef struct { int in_use; } CommerceCtx;
static CommerceCtx s_ctx[MAX_COMMERCE_CTX];

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 sceNpCommerce2Init(void)
{
    printf("[sceNpCommerce] Init()\n");
    if (s_initialized)
        return (s32)SCE_NP_COMMERCE2_ERROR_ALREADY_INITIALIZED;
    memset(s_ctx, 0, sizeof(s_ctx));
    s_initialized = 1;
    return CELL_OK;
}

s32 sceNpCommerce2Term(void)
{
    printf("[sceNpCommerce] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 sceNpCommerce2CreateCtx(u32 version, const char* npCommId,
                              SceNpCommerce2Ctx* ctx)
{
    (void)version; (void)npCommId;
    printf("[sceNpCommerce] CreateCtx()\n");

    if (!s_initialized) return (s32)SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED;
    if (!ctx) return (s32)SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < MAX_COMMERCE_CTX; i++) {
        if (!s_ctx[i].in_use) {
            s_ctx[i].in_use = 1;
            *ctx = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)SCE_NP_COMMERCE2_ERROR_INSUFFICIENT;
}

s32 sceNpCommerce2DestroyCtx(SceNpCommerce2Ctx ctx)
{
    printf("[sceNpCommerce] DestroyCtx(%u)\n", ctx);
    if (ctx < MAX_COMMERCE_CTX) s_ctx[ctx].in_use = 0;
    return CELL_OK;
}

s32 sceNpCommerce2GetCategoryContentsStart(SceNpCommerce2Ctx ctx,
                                             const char* categoryId,
                                             u32 startPosition, u32 maxEntries)
{
    (void)ctx; (void)categoryId; (void)startPosition; (void)maxEntries;
    printf("[sceNpCommerce] GetCategoryContentsStart()\n");
    return (s32)SCE_NP_COMMERCE2_ERROR_NOT_CONNECTED;
}

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

s32 sceNpCommerce2DoCheckoutFinishAsync(SceNpCommerce2Ctx ctx)
{
    (void)ctx;
    return CELL_OK;
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

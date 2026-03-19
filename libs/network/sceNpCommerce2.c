/*
 * ps3recomp - sceNpCommerce2 HLE implementation
 *
 * PlayStation Store commerce API stub. All operations return "not connected"
 * or "server error" since we don't connect to PSN.
 */

#include "sceNpCommerce2.h"
#include <stdio.h>
#include <string.h>

static int s_initialized = 0;

#define MAX_CTX 4
static int s_ctx_in_use[MAX_CTX];

s32 sceNpCommerce2Init(void)
{
    printf("[sceNpCommerce2] Init()\n");
    s_initialized = 1;
    memset(s_ctx_in_use, 0, sizeof(s_ctx_in_use));
    return CELL_OK;
}

s32 sceNpCommerce2Term(void)
{
    printf("[sceNpCommerce2] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 sceNpCommerce2CreateCtx(u32 version, const void* npId,
                              SceNpCommerce2Context* ctx)
{
    printf("[sceNpCommerce2] CreateCtx(version=%u)\n", version);
    if (!ctx) return SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < MAX_CTX; i++) {
        if (!s_ctx_in_use[i]) {
            s_ctx_in_use[i] = 1;
            *ctx = (u32)i;
            return CELL_OK;
        }
    }
    return SCE_NP_COMMERCE2_ERROR_SERVER_ERROR;
}

s32 sceNpCommerce2DestroyCtx(SceNpCommerce2Context ctx)
{
    if (ctx < MAX_CTX) s_ctx_in_use[ctx] = 0;
    return CELL_OK;
}

s32 sceNpCommerce2CreateSessionStart(SceNpCommerce2Context ctx)
{
    (void)ctx;
    printf("[sceNpCommerce2] CreateSessionStart — returning server error\n");
    return SCE_NP_COMMERCE2_ERROR_SERVER_ERROR;
}

s32 sceNpCommerce2CreateSessionFinish(SceNpCommerce2Context ctx)
{
    (void)ctx;
    return CELL_OK;
}

s32 sceNpCommerce2GetSessionInfo(SceNpCommerce2Context ctx, void* info)
{
    (void)ctx;
    if (info) memset(info, 0, 64); /* zero out info struct */
    return CELL_OK;
}

s32 sceNpCommerce2GetCategoryContentsStart(SceNpCommerce2Context ctx,
                                             const char* categoryId,
                                             u32 startPosition, u32 maxItems)
{
    (void)ctx; (void)categoryId; (void)startPosition; (void)maxItems;
    return SCE_NP_COMMERCE2_ERROR_SERVER_ERROR;
}

s32 sceNpCommerce2GetCategoryContentsFinish(SceNpCommerce2Context ctx)
{
    (void)ctx;
    return CELL_OK;
}

s32 sceNpCommerce2GetCategoryContentsGetResult(SceNpCommerce2Context ctx,
                                                 void* result)
{
    (void)ctx;
    if (result) memset(result, 0, 64);
    return SCE_NP_COMMERCE2_ERROR_SERVER_ERROR;
}

s32 sceNpCommerce2GetProductInfoStart(SceNpCommerce2Context ctx,
                                        const char* productId)
{
    (void)ctx; (void)productId;
    return SCE_NP_COMMERCE2_ERROR_SERVER_ERROR;
}

s32 sceNpCommerce2GetProductInfoFinish(SceNpCommerce2Context ctx)
{
    (void)ctx;
    return CELL_OK;
}

s32 sceNpCommerce2GetProductInfoGetResult(SceNpCommerce2Context ctx,
                                            void* result)
{
    (void)ctx;
    if (result) memset(result, 0, 64);
    return SCE_NP_COMMERCE2_ERROR_SERVER_ERROR;
}

s32 sceNpCommerce2AbortReq(SceNpCommerce2Context ctx)
{
    (void)ctx;
    return CELL_OK;
}

s32 sceNpCommerce2DestroyReq(SceNpCommerce2Context ctx)
{
    (void)ctx;
    return CELL_OK;
}

s32 sceNpCommerce2DoCheckoutFinishAsync(SceNpCommerce2Context ctx)
{
    (void)ctx;
    return SCE_NP_COMMERCE2_ERROR_SERVER_ERROR;
}

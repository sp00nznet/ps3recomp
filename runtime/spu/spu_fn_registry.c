/* spu_fn_registry.c — see spu_fn_registry.h. Extracted from spu_channels.c so
 * the registry + self-modification eviction is self-contained and unit-testable
 * (spu_channels.c pulls in the whole MFC/DMA world). */
#include "spu_fn_registry.h"
#include "spu_interp.h"   /* spu_lifted_lookup override */

typedef struct {
    uint32_t addr;
    spu_fn   fn;
    int      image_id;
    uint8_t  evicted;   /* self-modified since lift -> force interpretation */
} spu_reg_entry;

#define SPU_FN_REGISTRY_MAX 65536
static spu_reg_entry s_registry[SPU_FN_REGISTRY_MAX];
static uint32_t s_registry_count = 0;
static int s_reg_image = 0;

/* Lifted code segment [lo, hi). The store watch in spu_ls_write128 gates on
 * g_spu_code_hi (0 = inactive, free). Definitions live here (the registry owns
 * the code map); declared extern in spu_context.h. */
uint32_t g_spu_code_lo = 0xFFFFFFFFu;
uint32_t g_spu_code_hi = 0;

void spu_set_code_region(uint32_t lo, uint32_t hi)
{
    if (lo < g_spu_code_lo) g_spu_code_lo = lo;
    if (hi > g_spu_code_hi) g_spu_code_hi = hi;
}

void spu_begin_image(int image_id) { s_reg_image = image_id; }

void spu_register_function(uint32_t addr, spu_fn fn)
{
    if (s_registry_count < SPU_FN_REGISTRY_MAX) {
        s_registry[s_registry_count].addr     = addr;
        s_registry[s_registry_count].fn       = fn;
        s_registry[s_registry_count].image_id = s_reg_image;
        s_registry[s_registry_count].evicted  = 0;
        s_registry_count++;
        spu_set_code_region(addr, addr + 4);   /* auto-cover at least the entry */
    }
}

spu_fn spu_lookup(uint32_t addr, int image_id)
{
    /* Linear scan is fine for the small per-image tables. Match the context's
     * active image; image_id 0 (context or entry) matches any, for back-compat
     * with single-image contexts. Evicted entries never match -> caller (the
     * dispatcher) interprets the modified bytes from live local store. */
    for (uint32_t i = 0; i < s_registry_count; i++)
        if (!s_registry[i].evicted && s_registry[i].addr == addr &&
            (image_id == 0 || s_registry[i].image_id == 0 ||
             s_registry[i].image_id == image_id))
            return s_registry[i].fn;
    return NULL;
}

/* A function's body runs from its entry up to the next entry in the SAME image
 * (images overlap in LS, so a different image's entry in between does not bound
 * it). Using only same-image successors yields a >= extent, i.e. we err toward
 * over-eviction, which is the safe direction. */
static uint32_t entry_extent(uint32_t i)
{
    uint32_t flo = s_registry[i].addr, fhi = SPU_LS_SIZE;
    int img = s_registry[i].image_id;
    for (uint32_t j = 0; j < s_registry_count; j++) {
        uint32_t a = s_registry[j].addr;
        if (a > flo && a < fhi && s_registry[j].image_id == img)
            fhi = a;
    }
    return fhi;
}

void spu_invalidate(uint32_t lsa, uint32_t size)
{
    if (size == 0) return;
    uint32_t whi = lsa + size;              /* [lsa, whi) written */
    for (uint32_t i = 0; i < s_registry_count; i++) {
        if (s_registry[i].evicted) continue;
        uint32_t flo = s_registry[i].addr, fhi = entry_extent(i);
        if (flo < whi && lsa < fhi)         /* ranges overlap */
            s_registry[i].evicted = 1;
    }
}

uint32_t spu_registry_evicted_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_registry_count; i++) n += s_registry[i].evicted;
    return n;
}

/* Runtime write-watch entry point: a store (spu_ls_write128) or a DMA transfer
 * landed at [lsa, lsa+size). If it overlaps the lifted code segment, invalidate
 * the affected function(s). The spu_ls_write128 gate already ensured lsa is
 * below g_spu_code_hi; re-check the lower bound and delegate the precise
 * per-function overlap test to spu_invalidate. */
void spu_code_write_watch(uint32_t lsa, uint32_t size)
{
    if (lsa + size > g_spu_code_lo && lsa < g_spu_code_hi)
        spu_invalidate(lsa, size);
}

/* Strong override of spu_interp.c's weak stub: the interpreter's fast-path
 * lookup IS the registry lookup, so eviction is honored automatically. */
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa)
{
    return (spu_lifted_fn)spu_lookup(lsa, ctx ? ctx->image_id : 0);
}

/* spu_evict_selftest.c — self-modification eviction (spu_fn_registry.c).
 *
 * Registers three lifted functions with contiguous LS bodies, then simulates
 * sage's write-watch firing spu_invalidate() on a store into one body, and
 * asserts precisely that function's lookup now MISSES (so dispatch would
 * interpret it) while its neighbours keep their fast path.
 */
#include "../spu_fn_registry.h"
#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void fnA(spu_context* c) { (void)c; }
static void fnB(spu_context* c) { (void)c; }
static void fnC(spu_context* c) { (void)c; }

int main(void) {
    spu_begin_image(1);
    spu_register_function(0x100, fnA);   /* body [0x100,0x200) */
    spu_register_function(0x200, fnB);   /* body [0x200,0x300) */
    spu_register_function(0x300, fnC);   /* body [0x300, LS end) */

    assert(spu_lookup(0x100, 1) == fnA);
    assert(spu_lookup(0x200, 1) == fnB);
    assert(spu_lookup(0x300, 1) == fnC);

    /* Write into the MIDDLE of fnB's body — not its entry. Precise eviction
     * must still catch it (self-mod rarely lands on the entry word). */
    spu_invalidate(0x210, 4);
    assert(spu_lookup(0x200, 1) == NULL);   /* evicted -> interpret */
    assert(spu_lookup(0x100, 1) == fnA);    /* neighbour untouched */
    assert(spu_lookup(0x300, 1) == fnC);
    assert(spu_registry_evicted_count() == 1);

    /* A write spanning the fnC boundary evicts fnC too (fnB already gone). */
    spu_invalidate(0x2F0, 0x20);            /* [0x2F0,0x310): fnB + fnC */
    assert(spu_lookup(0x300, 1) == NULL);
    assert(spu_registry_evicted_count() == 2);

    /* The interpreter's fast-path hook honors eviction automatically. */
    static spu_context ctx; memset(&ctx, 0, sizeof ctx); ctx.image_id = 1;
    assert(spu_lifted_lookup(&ctx, 0x100) == (spu_lifted_fn)fnA);
    assert(spu_lifted_lookup(&ctx, 0x200) == NULL);

    printf("spu_evict_selftest: PASS (evicted=%u)\n", spu_registry_evicted_count());
    return 0;
}

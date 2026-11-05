/* Standalone fixtures that include spu_context.h without spu_channels.c own
 * the LS-watch service explicitly. No watch is armed in these tests. Keep a
 * fail-fast slow path so an unexpected watch call cannot silently succeed.
 * Include in exactly one fixture translation unit; never in the runtime. */
#ifndef PS3RECOMP_TEST_SPU_LS_WATCH_FIXTURE_H
#define PS3RECOMP_TEST_SPU_LS_WATCH_FIXTURE_H
#include <stdint.h>
#include <stdlib.h>

int g_spu_ls_watch_n = 0;
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p,
                       uint32_t pc, uint32_t lr)
{
    (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr;
    abort();
}
#endif

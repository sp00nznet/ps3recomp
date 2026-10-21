/* mfc_for runs on every MFC channel read and write. It must not look an environment variable up each time: with
 * SPU_PCSAMPLE unset it used to, and getenv walks the whole environment, so an SPU program that talks to its MFC in a
 * tight loop (an audio DSP thread) ran at half speed. spu_channels.c's getenv calls are counted by name. */
#include <stdlib.h>
#include <string.h>

static int g_pcsample_lookups;

static char* counting_getenv(const char* name)
{
    if (strcmp(name, "SPU_PCSAMPLE") == 0) g_pcsample_lookups++;
    return getenv(name);
}

#define getenv counting_getenv
#define main registry_previous_main
#include "test_spu_mfc_slots.c"
#undef main
#undef getenv

#define ACCESSES 100000

int main(void)
{
    unsetenv("SPU_PCSAMPLE");
    static spu_context ctx;
    mfc_engine* first = mfc_for(&ctx);
    int same = 1;
    for (int i = 1; i < ACCESSES; i++)
        same &= mfc_for(&ctx) == first;
    spu_mfc_release(&ctx);

    CHECK(same);
    CHECK(g_pcsample_lookups <= 1);
    printf("SPU_PCSAMPLE looked up %d time(s) over %d MFC accesses: %d passed, %d failed\n",
           g_pcsample_lookups, ACCESSES, g_pass, g_fail);
    return g_fail ? 1 : 0;
}

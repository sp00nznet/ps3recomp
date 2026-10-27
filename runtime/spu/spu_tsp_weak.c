/* Default stub for the YDKJ lifted SPU taskset-policy entry.
 *
 * spu_workload.c references tsp_spu_func_00000A00 inside a YDKJ_CRI_TASKSET
 * diagnostic branch. The real definition is generated per-game (ydkj's lifted
 * SPU policy module) and only that port compiles it; every other port still
 * needs the symbol to LINK. This stub satisfies the link and screams if it is
 * ever actually reached without the real module. A port that ships the real
 * lifted module must exclude this TU (see root CMakeLists EXCLUDE regex).
 * (Plain definition, not __attribute__((weak)): the runtime lib builds under
 * MSVC, which has no GNU weak attribute.) */
#include <stdio.h>

struct spu_context;

void tsp_spu_func_00000A00(struct spu_context* ctx)
{
    (void)ctx;
    fprintf(stderr, "[spu] tsp_spu_func_00000A00 stub called -- this port "
                    "was built without the ydkj lifted taskset policy module\n");
    fflush(stderr);
}

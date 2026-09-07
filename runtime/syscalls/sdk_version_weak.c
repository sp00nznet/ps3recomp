/* Default for the SDK version sys_process_get_sdk_version reports.
 *
 * The real number comes out of the title: a loader reads
 * sys_process_param_t.sdk_version from the ELF's PROC_PARAM segment and assigns
 * it before running any guest code. A port that does that defines this variable
 * itself, and every other port still needs the symbol to LINK.
 *
 * Separate one-symbol TU on purpose: a port that ships its own definition
 * resolves the reference from its own object and this archive member is never
 * pulled in, so no duplicate-symbol error. Same pattern as
 * runtime/syscalls/lbp_jobdone_weak.c and runtime/spu/spu_tsp_weak.c. (Plain
 * definition, not __attribute__((weak)): the runtime lib builds under MSVC,
 * which has no GNU weak attribute.)
 *
 * The value is what a title with no PROC_PARAM segment gets. It is not a
 * neutral choice -- libsre picks feature sets off it -- so it is a real SDK
 * version rather than zero. */
#include <stdint.h>

uint32_t g_ps3_sdk_version = 0x00350001;

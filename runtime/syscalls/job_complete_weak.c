/* Default stub for the per-port SPU-job-completion hook.
 *
 * sys_semaphore.c calls ps3_spu_job_complete_pending() on every semaphore wait/trywait
 * (an engine job manager can spin on semaphores waiting for SPU-job completions a
 * lifted policy module never writes). The real definition is per-port -- lbp/main.cpp
 * is the one that has it today; every other port still needs the symbol to LINK.
 *
 * Separate one-symbol TU on purpose: a port that ships the real definition resolves
 * the reference from its own object and this archive member is never pulled in, so
 * no duplicate-symbol error. Same pattern as runtime/spu/spu_tsp_weak.c. (Plain
 * definition, not __attribute__((weak)): the runtime lib builds under MSVC, which
 * has no GNU weak attribute.) */

void ps3_spu_job_complete_pending(void)
{
    /* No pending-job bookkeeping in this port -- nothing to complete. */
}

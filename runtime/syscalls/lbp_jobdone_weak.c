/* Default stub for the LBP job-completion hook.
 *
 * sys_semaphore.c calls lbp_hle_complete_pending() on every semaphore wait/trywait
 * (the LBP_HLE_JOBDONE path: LBP's JobManagerWorker spins on semaphores waiting for
 * SPU-job completions the lifted PM never writes). The real definition is per-game
 * (lbp/main.cpp); every other port still needs the symbol to LINK.
 *
 * Separate one-symbol TU on purpose: a port that ships the real definition resolves
 * the reference from its own object and this archive member is never pulled in, so
 * no duplicate-symbol error. Same pattern as runtime/spu/spu_tsp_weak.c. (Plain
 * definition, not __attribute__((weak)): the runtime lib builds under MSVC, which
 * has no GNU weak attribute.) */

void lbp_hle_complete_pending(void)
{
    /* No pending-job bookkeeping outside the LBP port -- nothing to complete. */
}

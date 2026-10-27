/* spu_lifted_fallback.c — lv2 glue: run a LIFTED SPU job as a PPU-fallback.
 *
 * The lv2 SPU-thread-group layer (lv2_register.c) runs each SPU thread by looking
 * up a PPU-fallback for its image entry point and calling it on a host thread with
 * (tid, args_ea, args_size, user). Register this wrapper with `user` = the lifted
 * entry fn (from spu_lifter), and lv2 will execute the lifted SPU code on the
 * thread's local store with the SPURS task arg in r3:
 *
 *     spu_register_ppu_fallback(image_entry, spu_lifted_fallback, (void*)lifted_fn);
 *
 * This bridges the lv2 SPU-thread layer to the lifted-execution layer — the brick
 * the flOw SPU integration needs (feed lifted jobs as task bodies).
 */
#include "../spu/spu_lifted_job.h"
#include "../spu/spu_workload.h"
#include <stdint.h>

/* defined in lv2_register.c — the SPU thread's 256 KB local store */
extern uint8_t* spu_thread_get_local_store(uint32_t tid);
extern uint32_t spu_thread_get_group_id(uint32_t tid);
extern uint32_t spu_thread_take_pending_inmbox(uint32_t tid);

int32_t spu_lifted_fallback(uint32_t tid, uint32_t args_ea,
                            uint32_t args_size, void* user)
{
    (void)args_size;
    return spu_run_lifted_job((spu_lifted_entry_fn)user,
                              spu_thread_get_local_store(tid), args_ea);
}

/* As above, but the lifted entry is found at run time by CONTENT FINGERPRINT
 * rather than by image entry point. Two registries existed and the raw
 * sys_spu_thread_group path only ever consulted the entry-point one, so a title
 * that starts a plain SPU thread group -- Virtua Fighter 5's "CriSr thread
 * group", and it imports no cellSpurs at all -- reported "no fallback" with its
 * image lifted and registered the whole time.
 *
 * `user` carries the fingerprint. spurs_task_abi is 0: a raw SPU thread is
 * entered with its argument EA in r3, not the SPURS task descriptor. */
int32_t spu_registry_fallback(uint32_t tid, uint32_t args_ea,
                              uint32_t args_size, void* user)
{
    (void)args_size;
    int image_id = 0;
    spu_lifted_entry_fn fn =
        spu_workload_find_img((uint64_t)(uintptr_t)user, &image_id);
    if (!fn) return -1;
    /* Raw SPU thread, not a SPURS job: it is a persistent worker. Arm parking so
     * it halts at its idle mailbox poll instead of spinning, hand it any command
     * the PPU wrote with sys_spu_thread_write_spu_mb, and identify the context so
     * its reply can be routed back to the connected event queue. */
    /* Block on an empty inbound mailbox rather than parking. g_spu_force_ch_block
     * is exactly the switch spu_raw.c uses for the same situation: a raw SPU on
     * its own host thread whose mailboxes the PPU pokes. A blocking rdch keeps
     * this thread's C stack (and therefore the SPU's registers) alive across the
     * wait, which park-and-restart cannot do. */
    { extern int g_spu_force_ch_block; g_spu_force_ch_block = 1; }
    spu_run_opts opts;
    opts.inmbox_val    = spu_thread_take_pending_inmbox(tid);
    opts.park_on_empty = 0;
    opts.spu_id        = tid;
    opts.group_id      = spu_thread_get_group_id(tid);
    return spu_run_lifted_job_abi(fn, spu_thread_get_local_store(tid), args_ea,
                                  image_id, 0, NULL, &opts);
}


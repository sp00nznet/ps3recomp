/* spu_lifted_thread.h -- run an SPU thread's own code, lifted, on a host thread.
 *
 * sys_spu_thread_group_start had two ways to run a thread: a PPU fallback
 * registered against the image's entry point, and the SPU interpreter. This is
 * the third and the faithful one. When a title has registered lifted code for
 * the image a thread was initialized with (spu_register_function, keyed on the
 * LS address and the image id), the thread runs THAT: a real spu_context whose
 * local store holds the image's segments, whose r3..r6 hold the arguments lv2
 * copied at sys_spu_thread_initialize, and whose pc is the image's entry.
 *
 * The split with lv2 is deliberate. lv2_register.c owns the thread group state
 * machine, the host threads and the completion events, which are the same for
 * a fallback thread and a lifted one. Everything SPU about starting a thread
 * lives here: laying the image into local store, the argument registers, which
 * image the registry should dispatch in, and what the SPU meant by the stop
 * instruction it finished on. That also makes the path testable without a
 * guest -- see runtime/spu/tests/test_spu_lifted_start.c.
 */
#ifndef SPU_LIFTED_THREAD_H
#define SPU_LIFTED_THREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;

/* What lv2 knows about an SPU thread by the time its group starts. */
typedef struct spu_lifted_thread_desc {
    uint32_t tid;        /* thread id handed out by sys_spu_thread_initialize */
    uint32_t group_id;   /* parent thread group                               */
    uint32_t entry;      /* LS entry point, read from the sys_spu_image       */
    uint32_t img_ea;     /* guest sys_spu_image; 0 deploys nothing            */
    uint64_t args[4];    /* sys_spu_thread_argument, as captured at initialize */
} spu_lifted_thread_desc;

/* How the thread stopped, in the terms the group state machine needs. */
typedef struct spu_lifted_thread_result {
    int32_t exit_status;    /* this thread's status                           */
    int     group_exit;     /* the SPU asked the whole GROUP to exit          */
    int32_t group_status;   /* ...with this status                            */
    int     faulted;        /* stopped outside the documented exit protocol   */
} spu_lifted_thread_result;

/* Is there lifted code for an image whose entry point is `entry`? This is what
 * decides between real SPU execution and the fallback/interpreter paths. */
int spu_lifted_thread_available(uint32_t entry);

/* Bring `ctx` up to the state lv2 hands a freshly started SPU thread: image
 * deployed into local store, arguments in r3..r6, pc at the entry, and the
 * image id the registry keys dispatch on. Zeroes `ctx` first, so it may be a
 * context an earlier run left behind. */
void spu_lifted_thread_setup(struct spu_context* ctx,
                             const spu_lifted_thread_desc* d);

/* Run a context prepared by spu_lifted_thread_setup until the SPU stops, and
 * classify the stop into `out`. Call this ON the host thread the SPU thread
 * owns: the halt landing pad and the transfer trampoline are thread-local, so
 * a context is pinned to one host thread for the length of its run. */
void spu_lifted_thread_run(struct spu_context* ctx,
                           spu_lifted_thread_result* out);

#ifdef __cplusplus
}
#endif

#endif /* SPU_LIFTED_THREAD_H */

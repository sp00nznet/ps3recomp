/*
 * ps3recomp - the CellSpurs instance: layout and the SPU-side kernel
 *
 * The CellSpurs instance (CELL_SPURS_SIZE = 4096 bytes of game-owned guest
 * memory, 128-byte aligned) is shared state between three parties: the PPU
 * library (cellSpurs.c), the SPURS kernel on each SPU (spurs_kernel.c), and
 * the title itself, whose inlined SDK code stores readyCounts and signal bits
 * directly. It therefore holds the real big-endian layout firmware libsre
 * uses; offsets below are the ones libsre reads and writes (cross-checked by
 * tests/conformance/spurs against libsre run by RPCS3).
 */
#ifndef PS3RECOMP_SPURS_INSTANCE_H
#define PS3RECOMP_SPURS_INSTANCE_H

#include <stdint.h>

enum {
    /* scheduling line 0 (0x00..0x7F) */
    SPURS_WKL_READY1     = 0x00,   /* u8/wid: SPUs requested (readyCount) */
    SPURS_WKL_IDLE2      = 0x10,   /* u8/wid: idle SPUs requested */
    SPURS_WKL_CURCONT    = 0x20,   /* u8/wid: SPUs running it now */
    SPURS_WKL_PENDCONT   = 0x30,   /* u8/wid: SPUs about to switch to it */
    SPURS_WKL_MINCONT    = 0x40,
    SPURS_WKL_MAXCONT    = 0x50,
    SPURS_WKL_FLAG       = 0x60,   /* CellSpursWorkloadFlag: be u32 flag at +0x0C */
    SPURS_WKL_SIGNAL1    = 0x70,   /* be u16: bit 15-wid */
    SPURS_SYSSRV_MESSAGE = 0x72,   /* u8: bit per SPU, "run the system service" */
    SPURS_SPU_IDLING     = 0x73,   /* u8: bit per SPU idling in the system service */
    SPURS_FLAGS1         = 0x74,   /* u8: SF1_* */
    SPURS_NSPUS          = 0x76,
    SPURS_WKL_FLAG_RCV   = 0x77,   /* u8: workload owning the flag, 0xFF none */
    /* state line 1 (0x80..0xFF) */
    SPURS_WKL_STATE1     = 0x80,   /* u8/wid: WKL_STATE_* */
    SPURS_WKL_STATUS1    = 0x90,   /* u8/wid: bit per SPU that has taken the workload */
    SPURS_WKL_EVENT1     = 0xA0,   /* u8/wid: 0x01 shutdown done, 0x02 has hook,
                                      0x10 a waiter, 0x20 hook called */
    SPURS_WKL_ENABLED    = 0xB0,   /* be u32: bit 31-wid; 0xFFFF low half on SPURS1 */
    SPURS_WKL_MSKB       = 0xB4,   /* be u32: wids with a policy module (uniqueId pool) */
    SPURS_SYSSRV_MSG     = 0xBD,   /* u8: bit per SPU, "re-read the workload table" */
    SPURS_SYSSRV_TERMINATE = 0xBF, /* u8: bit per SPU, "leave" */
    SPURS_SYSSRV_ON_SPU  = 0xC8,   /* u8: bit per SPU running the system service */
    SPURS_SPU_PORT       = 0xC9,   /* u8: the kernel's own SPU event port */
    SPURS_SYSSRV_TRACE_INIT = 0xCC,/* u8: bit per SPU whose system service set up tracing */
    /* per-workload blocks */
    SPURS_WKL_F1         = 0x100,  /* 0x80/wid: +0x28 waiter word, +0x30 hook, +0x38 hook arg */
    SPURS_WKL_F1_SZ      = 0x80,
    SPURS_WKL_INFO1      = 0xB00,  /* 0x20/wid: addr u64, arg u64, size u32, uniqueId u8 @0x14,
                                      priority[8] @0x18 */
    SPURS_WKL_INFO_SZ    = 0x20,
    SPURS_WKL_H1         = 0xE00,  /* 0x10/wid: nameClass u64, nameInstance u64 */
    /* configuration */
    SPURS_PPU0           = 0xD20,  /* be u64: handler threads */
    SPURS_PPU1           = 0xD28,
    SPURS_SPU_TG         = 0xD30,  /* be u32: SPU thread group id */
    SPURS_SPUS           = 0xD34,  /* be u32[8]: SPU thread ids */
    SPURS_CFG_FLAGS      = 0xD80,  /* be u32: attribute flags (SAF_*) */
    SPURS_SPU_PRIO       = 0xD84,
    SPURS_PPU_PRIO       = 0xD88,
    SPURS_PREFIX         = 0xD8C,  /* char[15], not NUL-terminated */
    SPURS_PREFIX_SIZE    = 0xD9B,
    SPURS_REVISION       = 0xDA0,
    SPURS_SDK_VERSION    = 0xDA4,
    SPURS_PORT_BITS      = 0xDA8,  /* be u64: SPU ports the app attached */
    SPURS_INST_SIZE      = 0x1000,
};

enum { WKL_STATE_NONE = 0, WKL_STATE_PREPARING = 1, WKL_STATE_RUNNABLE = 2,
       WKL_STATE_SHUTTING_DOWN = 3, WKL_STATE_REMOVABLE = 4 };

#define SPURS_SF1_EXIT_IF_NO_WORK 0x80u
#define SPURS_SYS_SERVICE_WID     0x20u      /* the kernel's own system workload */
#define SPURS_MAX_WKL             16u        /* SPURS1 */
#define SPURS_POLL_READYCOUNT     1u
#define SPURS_POLL_SIGNAL         2u
#define SPURS_POLL_FLAG           4u

/* ---- the SPU side (spurs_kernel.c) ---------------------------------------- */

/* Start the instance's SPU thread group: one kernel per SPU. */
void spurs_kernel_start(uint32_t spurs_ea, uint32_t nspus);
/* Terminate it (Finalize) and wait for every SPU to leave. */
void spurs_kernel_stop(uint32_t spurs_ea);
/* The PPU library changed the instance: wake idling SPUs now rather than at
 * their next poll (they also notice changes the title stores itself). */
void spurs_kernel_notify(uint32_t spurs_ea);
/* WakeUp: restart a group that exited for lack of work (exitIfNoWork). */
void spurs_kernel_wakeup(uint32_t spurs_ea);
/* The PPU side completed a shutdown itself (no SPU had the workload):
 * deliver it to the handler as the SPU side would. */
void spurs_kernel_shutdown_completed(uint32_t spurs_ea, uint32_t wid);
/* WaitForWorkloadShutdown's semaphore: block until the handler posts it. */
void spurs_kernel_wait_shutdown_sema(uint32_t spurs_ea, uint32_t wid);

#endif

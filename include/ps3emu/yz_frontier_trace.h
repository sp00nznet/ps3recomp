#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fixed-size, process-lifetime flight recorder for the post-movie completion
 * frontier.  The hot path is allocation-free and performs no formatting or
 * I/O.  YZ_FRONTIER_RING enables the dormant recorder; a semantic Job B
 * selection arms it.  YZ_FRONTIER_RING_PATH selects the dump-file prefix.
 */
enum yz_frontier_event_type {
    YZ_FT_ARM = 1,
    YZ_FT_PPU_SITE,
    YZ_FT_PPU_VALUE,
    YZ_FT_PPU_STORE,
    YZ_FT_SPU_JOB_SELECT,
    YZ_FT_SPU_DMA_CMD,
    YZ_FT_SPU_OUT_MBOX,
    YZ_FT_SPU_INTR_MBOX,
    YZ_FT_EVENT_WAIT,
    YZ_FT_EVENT_SET,
    YZ_FT_EVENT_QUEUE,
    YZ_FT_SPU_HALT,
    YZ_FT_RSX_STATE,
    YZ_FT_STALL,
    YZ_FT_FIFO_PARK,
    YZ_FT_FIFO_STATE,
    YZ_FT_FIFO_PUBLICATION,
    YZ_FT_SPU_STATE,
    YZ_FT_SPU_JOB_STATE,
    YZ_FT_SPU_MFC_STATE,
    YZ_FT_EVENT_STATE,
    /* Three-party dialogue-load handoff ordering (STATUS 2026-08-06 frontier).
     * YZ_FT_ATOMIC_COMMIT's value is also hardcoded as a numeric literal in
     * runtime/spu/spu_dma.h (that header cannot include this one — the gcc
     * runtime tests compile with -I runtime/spu only). Keep both in sync. */
    YZ_FT_TASK_WAIT,      /* 22: WAIT_SIGNAL park/wake. actor=wid pc=task
                           * a0=taskset ea, a1=phase (0=park, 1=wake-host,
                           * 2=wake-guest-latch, 3=shutdown), a2=guest latch
                           * byte, a3=host signalled flag */
    YZ_FT_TASK_SIGNAL,    /* 23: SendSignal delivery. actor=wid pc=task
                           * a0=taskset ea, a1=target waiting?, a2=srch-gate
                           * verdict (0=delivered, 1=dropped-SRCH) */
    YZ_FT_ATOMIC_COMMIT,  /* 24: SPU lock-line commit on an EA-trap-watched
                           * line. actor=spu pc=ls-pc, a0=line ea,
                           * a1=(img<<16)|(nchanged<<8)|first-off,
                           * a2=old word @first-off, a3=new word @first-off,
                           * a4=new word @second-off, a5=(cmd<<8)|second-off */
    YZ_FT_SYSCALL,        /* 25: actor=PPU tid, pc=syscall, a0=0 enter/1 exit,
                           * a1..a3=guest args, a4=return, a5=elapsed us */
    YZ_FT_PPU_THREAD,     /* 26: actor=tid, pc=CIA, a0=LR, a1=SP, a2=CTR,
                           * a3=wait syscall (0=guest), a4=object, a5=age ms */
    YZ_FT_PPU_STACK,      /* 27: actor=tid, pc=return CIA, a0=frame SP,
                           * a1=next SP, a2=depth */
    YZ_FT_SPURS_WORKLOAD, /* 28: actor=instance, pc=object EA,
                           * a0=active mask, a1=runnable mask, a2=nspus,
                           * a3=shutdown, a4=next wid, a5=next spu */
    YZ_FT_SPURS_TASKSET,  /* 29: actor=wid, pc=taskset EA, a0=mask word index,
                           * a1=running, a2=ready, a3=enabled, a4=signal,
                           * a5=waiting */
    YZ_FT_SPURS_TASK,     /* 30: actor=wid, pc=task id, a0=taskset EA,
                           * a1=host state flags, a2=context EA,
                           * a3=image, a4=exit code, a5=idle polls */
    YZ_FT_JOBCHAIN,       /* 31: actor=wid, pc=chain EA, a0=phase/reason,
                           * a1=current/slot EA, a2=descriptor EA,
                           * a3=value/kind, a4=aux, a5=state flags */
    YZ_FT_JOB,            /* 32: actor=wid, pc=phase (0 start/1 done),
                           * a0=descriptor, a1=binary, a2=d10,
                           * a3=post-d10, a4=result, a5=ticket/sequence */
    YZ_FT_COMPLETION,     /* 33: actor=tid/wid, pc=phase, a0=descriptor,
                           * a1=word EA, a2=actual, a3=expected,
                           * a4=adjacent ready value, a5=producer/decision */
    YZ_FT_RESERVATION,    /* 34: actor=spu/tid, pc=owner PC, a0=line EA,
                           * a1=active/phase, a2=value, a3=command/tag,
                           * a4=owner serial, a5=aux */
    YZ_FT_RELEASE_JOURNAL,/* 35: actor=tid, pc=phase, a0=state EA,
                           * a1=head, a2=pending, a3=cursor, a4=entry,
                           * a5=published value */
    YZ_FT_FRAME_CREDIT,   /* 36: a0=object, a1=credits, a2..a5=work objects */
    YZ_FT_GCM_CALLBACK,   /* 37: actor=tid, pc=phase, a0=context EA,
                           * a1=begin, a2=end, a3=current, a4=GET, a5=PUT */
    YZ_FT_AUTOPSY,        /* 38: pc=phase (1 soft/2 hard/3 logical),
                           * args are detector dependency signature */
    YZ_FT_LOGICAL_STATE,  /* 39: a0=frame, a1=input serial,
                           * a2=semantic serial, a3=worker serial,
                           * a4/a5=semantic state words */
    YZ_FT_SYNC_STATE,     /* 40: actor=object id, pc=kind (1 semaphore,
                           * 2 mutex, 3 cond, 4 rwlock, 5 queue, 6 flag),
                           * a0=actual, a1=limit/expected, a2=waiters,
                           * a3/a4=owner or association, a5=flags */
    YZ_FT_PARITY_FLIP,    /* 41: actor=buffer id, pc=target surface,
                           * a0=flip serial, a1=last draw generation,
                           * a2=last clear generation, a3=RSX GET,
                           * a4=RSX PUT, a5=current surface */
    YZ_FT_PARITY_MOTION,  /* 42: actor=wid, pc=phase (0 submit/1 complete),
                           * submit: a0=chain, a1=descriptor, a2=generation,
                           * a3=(image<<20)|slot, a4=ticket, a5=ticket value;
                           * complete: a0=chain, a1=descriptor, a2=submit gen,
                           * a3=completion gen, a4=published ticket value,
                           * a5=(image<<20)|slot */
    YZ_FT_PARITY_MOTION_DMA,/* 43: actor=motion submission generation,
                             * pc=SPU LS pc, a0=destination EA, a1=size,
                             * a2=sample fingerprint, a3=command/tag,
                             * a4=LSA, a5=image */
    YZ_FT_PARITY_SURFACE,   /* 44: actor=surface, pc=last write kind,
                             * a0=last write generation, a1=draw, a2=clear,
                             * a3=copy, a4=blit, a5=resolve generation */
    YZ_FT_PARITY_SURFACE_AUX,/* 45: actor=surface, pc=resource serial,
                              * a0=create generation, a1=other generation,
                              * a2=guest blit generation, a3=offset,
                              * a4=location, a5=last present-copy generation */
    YZ_FT_PARITY_RENDER,    /* 46: actor=target surface, pc=current surface,
                             * a0=groups executed, a1=ring drops,
                             * a2=vertex-CB bytes, a3=VB bytes,
                             * a4=descriptor tables, a5=present-copy serial */
    YZ_FT_PARITY_GUEST_BLIT,/* 47: actor=surface, pc=DMA location,
                             * a0=guest-blit generation, a1=relative offset,
                             * a2=value, a3=GET, a4=PUT,
                             * a5=last actual D3D surface-write generation */
    YZ_FT_PARITY_CB_RECYCLE /* 48: actor=frame, pc=last presented surface,
                             * a0=used bytes, a1=capacity, a2=block bytes,
                             * a3=groups executed, a4=GET, a5=PUT */
};

enum yz_frontier_queue_phase {
    YZ_FT_QUEUE_RECEIVE_ENTER = 1,
    YZ_FT_QUEUE_RECEIVE_POP,
    YZ_FT_QUEUE_PUSH_FULL,
    YZ_FT_QUEUE_PUSH_OK
};

int yz_frontier_trace_init(void);
int yz_frontier_trace_enabled(void);
int yz_frontier_trace_is_armed(void);

void yz_frontier_trace_arm(uint32_t actor, uint32_t pc,
                           uint32_t source_ea, uint32_t ls,
                           uint32_t image, uint32_t descriptor);

void yz_frontier_trace_emit(uint32_t type, uint32_t actor, uint32_t pc,
                            uint32_t a0, uint32_t a1, uint32_t a2,
                            uint32_t a3, uint32_t a4, uint32_t a5);

/* One-shot dump. Returns 1 only for the caller that performed the dump.
 * Mode '2' (armed at init): dumps are numbered and re-arm, and stall dumps
 * (reason 1/2) are SUPPRESSED until yz_frontier_trace_progress() has
 * reported bootstrap progress (boots 49-51: mid-bootstrap dump I/O is a
 * measured early-wedge perturbation suspect). Reason 3 = process exit,
 * never suppressed. */
int yz_frontier_trace_dump(uint32_t reason);

/* Monotonic progress marker (presented frame count) from the host frame
 * loop; gates stall dumps in mode '2'. */
void yz_frontier_trace_progress(uint32_t frame);

/*
 * One-shot, read-only snapshots taken by the watchdog immediately before a
 * stall dump. They append compact records to the same ring; they never
 * format, allocate, or write files themselves.
 */
void yz_frontier_fifo_snapshot(uint32_t get, uint32_t put);
void yz_frontier_spu_snapshot(void);
void yz_frontier_event_snapshot(void);
void yz_frontier_sync_snapshot(void);
void yz_frontier_spurs_snapshot(void);

#ifdef __cplusplus
}
#endif

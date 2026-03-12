/*
 * ps3recomp - cellFiber HLE
 *
 * Cooperative multitasking fibers. On PS3, fibers provide lightweight
 * user-level context switching within a single SPU or PPU thread.
 * Implementation uses OS fibers (Windows) or ucontext (POSIX).
 */

#ifndef PS3RECOMP_CELL_FIBER_H
#define PS3RECOMP_CELL_FIBER_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_FIBER_ERROR_AGAIN             0x80410801
#define CELL_FIBER_ERROR_INVAL             0x80410802
#define CELL_FIBER_ERROR_NOMEM             0x80410803
#define CELL_FIBER_ERROR_DEADLK            0x80410804
#define CELL_FIBER_ERROR_PERM              0x80410805
#define CELL_FIBER_ERROR_BUSY              0x80410806
#define CELL_FIBER_ERROR_ABORT             0x80410807
#define CELL_FIBER_ERROR_STAT              0x80410808
#define CELL_FIBER_ERROR_ALIGN             0x80410809
#define CELL_FIBER_ERROR_NULL_POINTER      0x8041080A

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define CELL_FIBER_DEFAULT_STACK_SIZE       (64 * 1024)  /* 64KB */
#define CELL_FIBER_MAX_FIBERS              64

/* Fiber states */
#define CELL_FIBER_STATE_INITIALIZED       0
#define CELL_FIBER_STATE_RUNNING           1
#define CELL_FIBER_STATE_SUSPENDED         2
#define CELL_FIBER_STATE_TERMINATED        3

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

/* Opaque fiber handle */
typedef u32 CellFiber;

/* Fiber function signature */
typedef void (*CellFiberEntry)(u64 arg);

/* Fiber attributes */
typedef struct CellFiberAttribute {
    const char* name;
    u32         priority;
    u32         stackSize;
} CellFiberAttribute;

/* ---------------------------------------------------------------------------
 * Scheduler (PPU fiber scheduler)
 * -----------------------------------------------------------------------*/

s32 cellFiberPpuInitialize(void);
s32 cellFiberPpuFinalize(void);

/* Fiber lifecycle */
s32 cellFiberPpuCreateFiber(CellFiber* fiber, CellFiberEntry entry,
                              u64 arg, const CellFiberAttribute* attr);
s32 cellFiberPpuDeleteFiber(CellFiber fiber);

/* Context switching */
s32 cellFiberPpuSwitchFiber(CellFiber fiber);
s32 cellFiberPpuYieldFiber(void);
s32 cellFiberPpuExitFiber(void);

/* Fiber queries */
s32 cellFiberPpuGetCurrentFiber(CellFiber* fiber);
s32 cellFiberPpuGetFiberState(CellFiber fiber, u32* state);

/* Attribute helpers */
s32 cellFiberPpuAttributeInitialize(CellFiberAttribute* attr);

/* Sleep / wake */
s32 cellFiberPpuSleep(void);
s32 cellFiberPpuWakeup(CellFiber fiber);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_FIBER_H */

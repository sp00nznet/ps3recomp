/*
 * ps3recomp - cellFiber HLE implementation
 *
 * Implements PPU fibers using Windows Fibers API or POSIX ucontext.
 * Each CellFiber maps to a native fiber for true cooperative switching.
 */

/* Darwin gates the ucontext routines AND the shape of ucontext_t itself on
 * _XOPEN_SOURCE, so this has to come before the first system header any
 * include below reaches -- see the static assertion further down for what
 * defining it late costs. Nothing above this line. */
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#  define _XOPEN_SOURCE 600
#endif

#include "cellFiber.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host pointer */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <ucontext.h>

#if defined(__APPLE__)
/* getcontext() points uc_mcontext at ucontext_t's own __mcontext_data member
 * and writes uc_mcsize (816) bytes there. That member is declared only when
 * _XOPEN_SOURCE was defined before <sys/_types/_ucontext.h> was first pulled
 * in; without it the struct is the 64-byte header alone and every getcontext,
 * swapcontext and makecontext writes 752 bytes off the end of it -- over
 * FiberSlot::stack, over the next FiberSlot in the array, and past
 * s_scheduler_context into whatever .bss follows. The symptom is not a crash
 * at the write but a fiber that starts with somebody else's argument, or a
 * slot that reports itself in use having never been created.
 *
 * The define above is the fix; this is the tripwire, because moving an
 * #include up here would silently undo it. */
_Static_assert(sizeof(ucontext_t) > 64,
               "ucontext_t has no embedded machine state: _XOPEN_SOURCE was "
               "defined too late and getcontext will write past the struct");
#endif
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct FiberSlot {
    int        in_use;
    u32        state;
    CellFiberEntry entry;
    u64        arg;
    u32        stack_size;
    char       name[32];
    u32        priority;
#ifdef _WIN32
    LPVOID     native_fiber;
#else
    ucontext_t context;
    u8*        stack;
#endif
} FiberSlot;

static FiberSlot s_fibers[CELL_FIBER_MAX_FIBERS];
static int s_initialized = 0;
static s32 s_current_fiber = -1; /* index into s_fibers, or -1 for scheduler */

#ifdef _WIN32
static LPVOID s_scheduler_fiber = NULL;
#else
static ucontext_t s_scheduler_context;
#endif

/* ---------------------------------------------------------------------------
 * Fiber trampoline
 * -----------------------------------------------------------------------*/

#ifdef _WIN32
static void CALLBACK fiber_trampoline(LPVOID param)
{
    u32 idx = (u32)(uintptr_t)param;
    FiberSlot* f = &s_fibers[idx];
    f->state = CELL_FIBER_STATE_RUNNING;
    f->entry(f->arg);
    f->state = CELL_FIBER_STATE_TERMINATED;

    /* Return to scheduler when fiber function returns */
    s_current_fiber = -1;
    SwitchToFiber(s_scheduler_fiber);
}
#else
static void fiber_trampoline(int idx_lo, int idx_hi)
{
    u32 idx = ((u32)idx_hi << 16) | (u32)(u16)idx_lo;
    FiberSlot* f = &s_fibers[idx];
    f->state = CELL_FIBER_STATE_RUNNING;
    f->entry(f->arg);
    f->state = CELL_FIBER_STATE_TERMINATED;
    s_current_fiber = -1;
    setcontext(&s_scheduler_context);
}
#endif

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellFiberPpuInitialize(void)
{
    printf("[cellFiber] Initialize()\n");

    if (s_initialized)
        return (s32)CELL_FIBER_ERROR_STAT;

    memset(s_fibers, 0, sizeof(s_fibers));
    s_current_fiber = -1;

#ifdef _WIN32
    /* Convert the current thread to a fiber so we can switch */
    s_scheduler_fiber = ConvertThreadToFiber(NULL);
    if (!s_scheduler_fiber) {
        /* Already a fiber (e.g., re-init) */
        s_scheduler_fiber = GetCurrentFiber();
    }
#else
    getcontext(&s_scheduler_context);
#endif

    s_initialized = 1;
    return CELL_OK;
}

s32 cellFiberPpuFinalize(void)
{
    printf("[cellFiber] Finalize()\n");

    if (!s_initialized)
        return (s32)CELL_FIBER_ERROR_STAT;

    for (int i = 0; i < CELL_FIBER_MAX_FIBERS; i++) {
        if (s_fibers[i].in_use) {
#ifdef _WIN32
            if (s_fibers[i].native_fiber)
                DeleteFiber(s_fibers[i].native_fiber);
#else
            free(s_fibers[i].stack);
#endif
            s_fibers[i].in_use = 0;
        }
    }

#ifdef _WIN32
    ConvertFiberToThread();
    s_scheduler_fiber = NULL;
#endif

    s_initialized = 0;
    return CELL_OK;
}

s32 cellFiberPpuCreateFiber(CellFiber* fiber, CellFiberEntry entry,
                              u64 arg, const CellFiberAttribute* attr)
{
    if (!s_initialized)
        return (s32)CELL_FIBER_ERROR_STAT;

    if (!fiber || !entry)
        return (s32)CELL_FIBER_ERROR_NULL_POINTER;

    /* Find a free slot */
    int idx = -1;
    for (int i = 0; i < CELL_FIBER_MAX_FIBERS; i++) {
        if (!s_fibers[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return (s32)CELL_FIBER_ERROR_NOMEM;

    FiberSlot* f = &s_fibers[idx];
    memset(f, 0, sizeof(FiberSlot));
    f->in_use = 1;
    f->entry = entry;
    f->arg = arg;
    f->state = CELL_FIBER_STATE_INITIALIZED;
    f->stack_size = CELL_FIBER_DEFAULT_STACK_SIZE;

    /* CellFiberAttribute is { const char* name; u32 priority; u32 stackSize; }.
     * The guest lays that out as three 4-byte fields; the host struct leads with
     * an 8-byte pointer, so a cast would read priority and stackSize from the
     * wrong offsets even after the address is translated. */
    if (attr) {
        u32 attr_ea    = (u32)(uintptr_t)attr;
        u32 name_ea    = vm_read32(attr_ea + 0);
        u32 priority   = vm_read32(attr_ea + 4);
        u32 stack_size = vm_read32(attr_ea + 8);
        if (stack_size > 0)
            f->stack_size = stack_size;
        if (name_ea)
            strncpy(f->name, (const char*)(vm_base + name_ea), sizeof(f->name) - 1);
        f->priority = priority;
    }

    printf("[cellFiber] CreateFiber(id=%d, name=%s, stack=%u)\n",
           idx, f->name[0] ? f->name : "(unnamed)", f->stack_size);

#ifdef _WIN32
    f->native_fiber = CreateFiber(f->stack_size, fiber_trampoline,
                                   (LPVOID)(uintptr_t)idx);
    if (!f->native_fiber) {
        f->in_use = 0;
        return (s32)CELL_FIBER_ERROR_NOMEM;
    }
#else
    f->stack = (u8*)malloc(f->stack_size);
    if (!f->stack) {
        f->in_use = 0;
        return (s32)CELL_FIBER_ERROR_NOMEM;
    }
    getcontext(&f->context);
    f->context.uc_stack.ss_sp = f->stack;
    f->context.uc_stack.ss_size = f->stack_size;
    f->context.uc_link = &s_scheduler_context;
    makecontext(&f->context, (void (*)(void))fiber_trampoline, 2,
                (int)(idx & 0xFFFF), (int)(idx >> 16));
#endif

    vm_write32((u32)(uintptr_t)fiber, (u32)idx);
    return CELL_OK;
}

s32 cellFiberPpuDeleteFiber(CellFiber fiber)
{
    if (!s_initialized)
        return (s32)CELL_FIBER_ERROR_STAT;

    u32 idx = fiber;
    if (idx >= CELL_FIBER_MAX_FIBERS || !s_fibers[idx].in_use)
        return (s32)CELL_FIBER_ERROR_INVAL;

    if (s_fibers[idx].state == CELL_FIBER_STATE_RUNNING)
        return (s32)CELL_FIBER_ERROR_BUSY;

    printf("[cellFiber] DeleteFiber(id=%u)\n", idx);

#ifdef _WIN32
    if (s_fibers[idx].native_fiber)
        DeleteFiber(s_fibers[idx].native_fiber);
#else
    free(s_fibers[idx].stack);
#endif

    s_fibers[idx].in_use = 0;
    return CELL_OK;
}

s32 cellFiberPpuSwitchFiber(CellFiber fiber)
{
    if (!s_initialized)
        return (s32)CELL_FIBER_ERROR_STAT;

    u32 idx = fiber;
    if (idx >= CELL_FIBER_MAX_FIBERS || !s_fibers[idx].in_use)
        return (s32)CELL_FIBER_ERROR_INVAL;

    if (s_fibers[idx].state == CELL_FIBER_STATE_TERMINATED)
        return (s32)CELL_FIBER_ERROR_STAT;

    /* Who is being switched away from, decided before s_current_fiber moves.
     * Both branches below need it and one of them used to read it after the
     * assignment, by which point it always said "the target". */
    s32 prev = s_current_fiber;

    /* Suspend current fiber if one is running */
    if (prev >= 0 && prev != (s32)idx)
        s_fibers[prev].state = CELL_FIBER_STATE_SUSPENDED;

    s_current_fiber = (s32)idx;
    s_fibers[idx].state = CELL_FIBER_STATE_RUNNING;

#ifdef _WIN32
    SwitchToFiber(s_fibers[idx].native_fiber);
#else
    /* SwitchToFiber saves the running fiber's state into the running fiber,
     * whichever one that is; swapcontext has to be told. A switch issued from
     * inside a fiber saves that fiber, and only a switch issued from the
     * scheduler saves the scheduler.
     *
     * The old selection tested s_current_fiber after assigning idx to it, so
     * "the caller is a different fiber" was never true and every switch saved
     * into s_scheduler_context. Two costs, both silent: the scheduler's own
     * saved context was overwritten by a fiber's, so the eventual return to
     * the scheduler resumed a fiber's stack instead of main's; and the
     * calling fiber's context was never updated, so switching back to it
     * restarted it from its entry point on the stack it was already using. */
    ucontext_t* from = (prev >= 0 && prev != (s32)idx)
                       ? &s_fibers[prev].context
                       : &s_scheduler_context;
    swapcontext(from, &s_fibers[idx].context);
#endif

    return CELL_OK;
}

s32 cellFiberPpuYieldFiber(void)
{
    if (!s_initialized || s_current_fiber < 0)
        return (s32)CELL_FIBER_ERROR_PERM;

    s_fibers[s_current_fiber].state = CELL_FIBER_STATE_SUSPENDED;
    s32 prev = s_current_fiber;
    s_current_fiber = -1;

#ifdef _WIN32
    SwitchToFiber(s_scheduler_fiber);
#else
    swapcontext(&s_fibers[prev].context, &s_scheduler_context);
#endif
    (void)prev;

    return CELL_OK;
}

s32 cellFiberPpuExitFiber(void)
{
    if (!s_initialized || s_current_fiber < 0)
        return (s32)CELL_FIBER_ERROR_PERM;

    s_fibers[s_current_fiber].state = CELL_FIBER_STATE_TERMINATED;
    s_current_fiber = -1;

#ifdef _WIN32
    SwitchToFiber(s_scheduler_fiber);
#else
    setcontext(&s_scheduler_context);
#endif

    return CELL_OK; /* unreachable */
}

s32 cellFiberPpuGetCurrentFiber(CellFiber* fiber)
{
    if (!fiber)
        return (s32)CELL_FIBER_ERROR_NULL_POINTER;

    if (s_current_fiber < 0)
        return (s32)CELL_FIBER_ERROR_PERM;

    vm_write32((u32)(uintptr_t)fiber, (u32)s_current_fiber);
    return CELL_OK;
}

s32 cellFiberPpuGetFiberState(CellFiber fiber, u32* state)
{
    u32 idx = fiber;
    if (idx >= CELL_FIBER_MAX_FIBERS || !s_fibers[idx].in_use)
        return (s32)CELL_FIBER_ERROR_INVAL;

    if (!state)
        return (s32)CELL_FIBER_ERROR_NULL_POINTER;

    vm_write32((u32)(uintptr_t)state, s_fibers[idx].state);
    return CELL_OK;
}

s32 cellFiberPpuAttributeInitialize(CellFiberAttribute* attr)
{
    if (!attr)
        return (s32)CELL_FIBER_ERROR_NULL_POINTER;

    u32 attr_ea = (u32)(uintptr_t)attr;
    vm_write32(attr_ea + 0, 0);                                  /* name      */
    vm_write32(attr_ea + 4, 0);                                  /* priority  */
    vm_write32(attr_ea + 8, CELL_FIBER_DEFAULT_STACK_SIZE);      /* stackSize */
    return CELL_OK;
}

s32 cellFiberPpuSleep(void)
{
    if (!s_initialized || s_current_fiber < 0)
        return (s32)CELL_FIBER_ERROR_PERM;

    printf("[cellFiber] Sleep(fiber=%d)\n", s_current_fiber);
    s_fibers[s_current_fiber].state = CELL_FIBER_STATE_SUSPENDED;
    s32 prev = s_current_fiber;
    s_current_fiber = -1;

#ifdef _WIN32
    SwitchToFiber(s_scheduler_fiber);
#else
    swapcontext(&s_fibers[prev].context, &s_scheduler_context);
#endif
    (void)prev;

    return CELL_OK;
}

s32 cellFiberPpuWakeup(CellFiber fiber)
{
    u32 idx = fiber;
    if (idx >= CELL_FIBER_MAX_FIBERS || !s_fibers[idx].in_use)
        return (s32)CELL_FIBER_ERROR_INVAL;

    if (s_fibers[idx].state != CELL_FIBER_STATE_SUSPENDED) {
        printf("[cellFiber] Wakeup(fiber=%u): not suspended (state=%u)\n",
               idx, s_fibers[idx].state);
        return (s32)CELL_FIBER_ERROR_STAT;
    }

    printf("[cellFiber] Wakeup(fiber=%u)\n", idx);
    s_fibers[idx].state = CELL_FIBER_STATE_INITIALIZED; /* ready to run */
    return CELL_OK;
}

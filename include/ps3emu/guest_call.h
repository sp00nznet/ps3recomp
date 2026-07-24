/*
 * ps3recomp — Guest callback dispatch hook
 *
 * The HLE runtime is plain C compiled to host. When a system service
 * (cellSysutil events, cellSaveData callbacks, etc.) needs to invoke a
 * GUEST-side callback the game registered, the runtime can't call it
 * directly — the callback's "function pointer" is actually a guest VM
 * address pointing at an OPD (Official Procedure Descriptor: 4 bytes
 * function entry, 4 bytes TOC, 4 bytes env).
 *
 * The recompiled code base owns the dispatcher (it has the giant
 * function table mapping guest_addr -> host_func and the trampoline
 * runner). It registers a `ps3_guest_caller_fn` that the HLE runtime
 * uses whenever it needs to fire a guest callback. This keeps the
 * runtime independent of any specific game's recompiled code.
 *
 * Calling convention for the hook: the guest function is invoked with
 * up to eight 64-bit args mapped to PPC r3..r10. The hook is responsible
 * for setting up a clean ppu_context, plumbing the args, dispatching
 * via OPD, and draining trampolines before returning.
 */

#ifndef PS3EMU_GUEST_CALL_H
#define PS3EMU_GUEST_CALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hook signature.
 *   opd_addr  – guest address of the OPD entry to invoke (3 x 32-bit)
 *   arg0..7   – first eight args, placed in r3..r10 by the caller
 *
 * The hook may run synchronously or queue + return; cellSysutilCheckCallback
 * is the typical synchronous dispatch site so games observe the callback
 * complete before returning. */
typedef void (*ps3_guest_caller_fn)(uint32_t opd_addr,
                                    uint64_t arg0, uint64_t arg1,
                                    uint64_t arg2, uint64_t arg3,
                                    uint64_t arg4, uint64_t arg5,
                                    uint64_t arg6, uint64_t arg7);

/* Set by the game's host code (e.g. flow main.cpp) at startup. NULL until
 * installed; HLE bridges that need to call back into guest code must
 * check for NULL before invoking. */
extern ps3_guest_caller_fn g_ps3_guest_caller;

/* Convenience wrapper — checks for NULL and invokes if set. */
static inline void ps3_invoke_guest(uint32_t opd_addr,
                                    uint64_t arg0, uint64_t arg1,
                                    uint64_t arg2, uint64_t arg3,
                                    uint64_t arg4, uint64_t arg5,
                                    uint64_t arg6, uint64_t arg7)
{
    if (g_ps3_guest_caller)
        g_ps3_guest_caller(opd_addr, arg0, arg1, arg2, arg3,
                           arg4, arg5, arg6, arg7);
}

#ifdef __cplusplus
}
#endif

#endif

/*
 * ps3recomp - cellSysutilNpEula HLE implementation
 *
 * sceNpEulaCheckEulaStatus asks the system whether the player has accepted
 * the title's PSN terms of service; the system answers later, through the
 * callback the title passed, on one of its cellSysutilCheckCallback polls.
 * None of the three exports was implemented, so the imports answered
 * CELL_OK and no callback ever came: a title that waits for the answer
 * behind a "Please wait..." box waits forever.
 *
 * The answer follows RPCS3 (rpcs3/Emu/Cell/Modules/cellSysutilNpEula.cpp):
 * one call at a time, the callback runs from the sysutil poll, and
 * sceNpEulaAbort turns the pending answer into ABORTED. Where it differs:
 *   - offline (the default: cellNetCtl reports no connection and the NP
 *     manager is OFFLINE) the terms cannot be fetched, so the answer is
 *     SCE_NP_EULA_ERROR, as a console without a connection gets. A title
 *     then leaves its online features alone. RPCS3's ALREADY_ACCEPTED would
 *     send it on to request a PSN ticket from a manager that is offline.
 *   - online (PS3_NET_ONLINE=1, or a psnr server) the answer is RPCS3's:
 *     ALREADY_ACCEPTED, version 1.
 *   - sceNpEulaShowCurrentEula (RPCS3: a TODO that never calls back) closes
 *     at once with the callback, so a "show the terms" menu entry does not
 *     leave its spinner up either.
 */

#include "cellSysutilNpEula.h"
#include "cellSysutil.h"          /* cellSysutilQueueGuestCallbackArgs */
#include <stdio.h>
#include <string.h>

int ps3_netctl_online(void);      /* libs/network/cellNetCtl.c */
int np_psnr_enabled(void);        /* libs/network/np_psnr.c */

#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_eula_lock = SRWLOCK_INIT;
#define EULA_LOCK()   AcquireSRWLockExclusive(&s_eula_lock)
#define EULA_UNLOCK() ReleaseSRWLockExclusive(&s_eula_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_eula_lock = PTHREAD_MUTEX_INITIALIZER;
#define EULA_LOCK()   pthread_mutex_lock(&s_eula_lock)
#define EULA_UNLOCK() pthread_mutex_unlock(&s_eula_lock)
#endif

enum { EULA_IDLE = 0, EULA_CHECK, EULA_SHOW };

static struct {
    int  kind;        /* EULA_IDLE: nothing pending */
    u32  status;      /* SceNpEulaStatus the callback will get */
    u32  error;
    u32  version;
    u32  cb;          /* guest OPD */
    u32  arg;
} s_eula;

static int eula_online(void) { return ps3_netctl_online() || np_psnr_enabled(); }

s32 sceNpEulaCheckEulaStatus(u32 communicationId, u32 arg2, u64 arg3,
                             u32 cbFunc, u32 cbFuncArg)
{
    if (!communicationId || !cbFunc) return (s32)SCE_NP_EULA_ERROR_INVALID_ARGUMENT;
    EULA_LOCK();
    if (s_eula.kind != EULA_IDLE) { EULA_UNLOCK(); return (s32)SCE_NP_EULA_ERROR_ALREADY_INITIALIZED; }
    const int online = eula_online();
    s_eula.kind    = EULA_CHECK;
    s_eula.status  = online ? SCE_NP_EULA_ALREADY_ACCEPTED : SCE_NP_EULA_ERROR;
    s_eula.error   = online ? 0u : SCE_NP_EULA_ERROR_UNKNOWN;
    s_eula.version = online ? 1u : 0u;
    s_eula.cb      = cbFunc;
    s_eula.arg     = cbFuncArg;
    EULA_UNLOCK();
    printf("[cellSysutilNpEula] CheckEulaStatus(arg2=0x%X, arg3=0x%llX, cb=0x%08X) -> %s\n",
           arg2, (unsigned long long)arg3, cbFunc,
           online ? "already accepted" : "error (offline: the terms cannot be fetched)");
    return CELL_OK;
}

s32 sceNpEulaAbort(void)
{
    EULA_LOCK();
    if (s_eula.kind == EULA_IDLE) { EULA_UNLOCK(); return (s32)SCE_NP_EULA_ERROR_NOT_INITIALIZED; }
    /* Nothing is on screen to take down: the pending answer becomes ABORTED. */
    if (s_eula.kind == EULA_CHECK) s_eula.status = SCE_NP_EULA_ABORTED;
    EULA_UNLOCK();
    printf("[cellSysutilNpEula] Abort()\n");
    return CELL_OK;
}

s32 sceNpEulaShowCurrentEula(u32 communicationId, u64 arg2, u32 cbFunc, u32 cbFuncArg)
{
    if (!communicationId || !cbFunc) return (s32)SCE_NP_EULA_ERROR_INVALID_ARGUMENT;
    EULA_LOCK();
    if (s_eula.kind != EULA_IDLE) { EULA_UNLOCK(); return (s32)SCE_NP_EULA_ERROR_ALREADY_INITIALIZED; }
    s_eula.kind   = EULA_SHOW;
    s_eula.status = 0; s_eula.error = 0; s_eula.version = 0;
    s_eula.cb     = cbFunc;
    s_eula.arg    = cbFuncArg;
    EULA_UNLOCK();
    printf("[cellSysutilNpEula] ShowCurrentEula(arg2=0x%llX, cb=0x%08X) -> closed (nothing to show)\n",
           (unsigned long long)arg2, cbFunc);
    return CELL_OK;
}

void ps3_np_eula_tick(void)
{
    EULA_LOCK();
    if (s_eula.kind == EULA_IDLE) { EULA_UNLOCK(); return; }
    u64 args[8] = {0};
    if (s_eula.kind == EULA_CHECK) {
        args[0] = s_eula.status; args[1] = s_eula.error;
        args[2] = s_eula.version; args[3] = s_eula.arg;
    } else {
        args[0] = 0; args[1] = 0; args[2] = s_eula.arg;
    }
    const u32 cb = s_eula.cb, status = s_eula.status;
    const int kind = s_eula.kind;
    /* Idle before the callback runs, so the title may ask again from it. */
    memset(&s_eula, 0, sizeof s_eula);
    EULA_UNLOCK();
    cellSysutilQueueGuestCallbackArgs(cb, args);
    if (kind == EULA_CHECK)
        printf("[cellSysutilNpEula] callback status=%u delivered\n", status);
}

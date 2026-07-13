/*
 * ps3recomp - cellGameExec HLE implementation
 *
 * Tracks boot/exit parameters. ExitToShelf terminates the recompiled process.
 */

#include "cellGameExec.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_write32 -- guest addrs */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* The generic HLE adapter passes GUEST addresses for pointer args; translate
 * to host pointers for struct/string access and write scalars big-endian. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static CellGameExecBootParam s_exit_param;
static int s_exit_param_set = 0;

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellGameSetExitParam(const CellGameExecBootParam* param_guest)
{
    const CellGameExecBootParam* param = GUEST_PTR(param_guest, const CellGameExecBootParam*);
    printf("[cellGameExec] SetExitParam(type=%u, titleId=%.16s)\n",
           param ? param->type : 0,
           param ? param->titleId : "(null)");

    if (!param)
        return CELL_EINVAL;

    s_exit_param = *param;
    s_exit_param_set = 1;
    return CELL_OK;
}

s32 cellGameGetExitParam(CellGameExecBootParam* param_guest)
{
    CellGameExecBootParam* param = GUEST_PTR(param_guest, CellGameExecBootParam*);
    printf("[cellGameExec] GetExitParam()\n");

    if (!param)
        return CELL_EINVAL;

    if (!s_exit_param_set)
        return CELL_ENOENT;

    *param = s_exit_param;
    return CELL_OK;
}

void cellGameExitToShelf(void)
{
    printf("[cellGameExec] ExitToShelf() -- terminating process\n");

    if (s_exit_param_set) {
        printf("[cellGameExec]   Next title: %.16s (type=%u)\n",
               s_exit_param.titleId, s_exit_param.type);
    }

    exit(0);
}

s32 cellGameGetBootGameInfo(u32* type, char* dirName, u32* execData)
{
    printf("[cellGameExec] GetBootGameInfo() -> DISC\n");

    /* Disc-boot title (matches cellGame's disc-content reporting). Args are
     * guest addresses: scalars written big-endian, dirName only meaningful
     * for HDD boot so it is zeroed. */
    if (type)
        vm_write32((uint32_t)(uintptr_t)type, CELL_GAME_GAMETYPE_DISC);

    if (dirName)
        memset(GUEST_PTR(dirName, char*), 0, 32);

    if (execData)
        vm_write32((uint32_t)(uintptr_t)execData, 0);

    return CELL_OK;
}

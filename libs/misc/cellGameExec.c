/*
 * ps3recomp - cellGameExec HLE implementation
 *
 * Tracks boot/exit parameters. ExitToShelf terminates the recompiled process.
 */

#include "cellGameExec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static CellGameExecBootParam s_exit_param;
static int s_exit_param_set = 0;

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellGameSetExitParam(const CellGameExecBootParam* param)
{
    printf("[cellGameExec] SetExitParam(type=%u, titleId=%.16s)\n",
           param ? param->type : 0,
           param ? param->titleId : "(null)");

    if (!param)
        return CELL_EINVAL;

    s_exit_param = *param;
    s_exit_param_set = 1;
    return CELL_OK;
}

s32 cellGameGetExitParam(CellGameExecBootParam* param)
{
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
    printf("[cellGameExec] GetBootGameInfo()\n");

    /* Report as HDD game by default */
    if (type)
        *type = CELL_GAME_GAMETYPE_HDD;

    if (dirName)
        strncpy(dirName, "GAME00000", 32);

    if (execData)
        *execData = 0;

    return CELL_OK;
}

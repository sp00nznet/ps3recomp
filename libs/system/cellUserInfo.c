/*
 * ps3recomp - cellUserInfo HLE implementation
 *
 * Reports a single default user (ID=00000001, name="User").
 */

#include "cellUserInfo.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* The emulated PS3 has one user */
#define EMU_USER_ID     CELL_USERINFO_DEFAULT_USER_ID
#define EMU_USER_NAME   "User"
#define EMU_HOME_DIR    "/dev_hdd0/home/00000001"

static u32 s_selected_user = EMU_USER_ID;

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellUserInfoGetStat(u32 id, CellUserInfoUserStat* stat)
{
    printf("[cellUserInfo] GetStat(id=%u)\n", id);

    if (!stat)
        return CELL_USERINFO_ERROR_PARAM;

    if (id != EMU_USER_ID)
        return CELL_USERINFO_ERROR_NOUSER;

    memset(stat, 0, sizeof(CellUserInfoUserStat));
    stat->id = EMU_USER_ID;
    strncpy(stat->name, EMU_USER_NAME, CELL_USERINFO_USERNAME_SIZE - 1);
    strncpy(stat->homeDir, EMU_HOME_DIR, CELL_USERINFO_HOME_PATH_SIZE - 1);

    return CELL_OK;
}

s32 cellUserInfoGetList(u32* listNum, CellUserInfoUserList* list, u32* currentUser)
{
    printf("[cellUserInfo] GetList()\n");

    if (listNum)
        *listNum = 1;

    if (list) {
        memset(list, 0, sizeof(CellUserInfoUserList));
        list->userId[0] = EMU_USER_ID;
    }

    if (currentUser)
        *currentUser = EMU_USER_ID;

    return CELL_OK;
}

s32 cellUserInfoSelectUser_ListSet(u32 listType,
                                    const CellUserInfoListSet* listSet,
                                    CellUserInfoFinishCallback callback,
                                    void* userdata)
{
    (void)listType;
    (void)listSet;
    printf("[cellUserInfo] SelectUser_ListSet()\n");

    /* Auto-select the default user */
    s_selected_user = EMU_USER_ID;

    /* Invoke callback immediately (synchronous selection) */
    if (callback)
        callback(CELL_OK, EMU_USER_ID, userdata);

    return CELL_OK;
}

s32 cellUserInfoSelectUser_ListGet(u32* selectedUser)
{
    printf("[cellUserInfo] SelectUser_ListGet()\n");

    if (!selectedUser)
        return CELL_USERINFO_ERROR_PARAM;

    *selectedUser = s_selected_user;
    return CELL_OK;
}

s32 cellUserInfoEnableOverlay(s32 enable)
{
    printf("[cellUserInfo] EnableOverlay(%d)\n", enable);
    /* No-op in recomp -- no system overlay */
    return CELL_OK;
}

s32 cellUserInfoGetHomeDir(u32 id, char* homePath, u32 homePathSize)
{
    printf("[cellUserInfo] GetHomeDir(id=%u)\n", id);

    if (!homePath || homePathSize == 0)
        return CELL_USERINFO_ERROR_PARAM;

    if (id != EMU_USER_ID)
        return CELL_USERINFO_ERROR_NOUSER;

    strncpy(homePath, EMU_HOME_DIR, homePathSize - 1);
    homePath[homePathSize - 1] = '\0';

    return CELL_OK;
}

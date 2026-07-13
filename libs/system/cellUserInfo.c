/*
 * ps3recomp - cellUserInfo HLE implementation
 *
 * Reports a single default user (ID=00000001, name="User").
 */

#include "cellUserInfo.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
/* HLE args arrive as guest effective addresses; translate before deref. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))
/* The guest is big-endian; u32 fields we write into guest structs must be stored
 * byte-swapped or the guest reads them mangled (e.g. a count of 1 read as
 * 0x01000000 = 16.7M, blowing up the caller's enumeration loop). */
#define BE32(v) ( (((uint32_t)(v) & 0x000000FFu) << 24) | (((uint32_t)(v) & 0x0000FF00u) << 8) | \
                  (((uint32_t)(v) & 0x00FF0000u) >> 8) | (((uint32_t)(v) & 0xFF000000u) >> 24) )

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
    if (!stat)
        return CELL_USERINFO_ERROR_PARAM;
    stat = GUEST_PTR(stat, CellUserInfoUserStat*);

    if (id != EMU_USER_ID)
        return CELL_USERINFO_ERROR_NOUSER;

    /* NOTE: write ONLY the real 68-byte struct (id+name). Anything more
     * overflows the guest's stack buffer (see header comment). */
    memset(stat, 0, sizeof(CellUserInfoUserStat));
    stat->id = BE32(EMU_USER_ID);
    strncpy(stat->name, EMU_USER_NAME, CELL_USERINFO_USERNAME_SIZE - 1);

    return CELL_OK;
}

s32 cellUserInfoGetList(u32* listNum, CellUserInfoUserList* list, u32* currentUser)
{
    printf("[cellUserInfo] GetList()\n");
    u32* listNum_h = GUEST_PTR(listNum, u32*);
    CellUserInfoUserList* list_h = GUEST_PTR(list, CellUserInfoUserList*);
    u32* currentUser_h = GUEST_PTR(currentUser, u32*);

    if (listNum_h)
        *listNum_h = BE32(1);

    if (list_h) {
        memset(list_h, 0, sizeof(CellUserInfoUserList));
        list_h->userId[0] = BE32(EMU_USER_ID);
    }

    if (currentUser_h)
        *currentUser_h = BE32(EMU_USER_ID);

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
    selectedUser = GUEST_PTR(selectedUser, u32*);

    *selectedUser = BE32(s_selected_user);
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
    homePath = GUEST_PTR(homePath, char*);

    if (id != EMU_USER_ID)
        return CELL_USERINFO_ERROR_NOUSER;

    strncpy(homePath, EMU_HOME_DIR, homePathSize - 1);
    homePath[homePathSize - 1] = '\0';

    return CELL_OK;
}

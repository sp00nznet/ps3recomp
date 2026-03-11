/*
 * ps3recomp - cellUserInfo HLE
 *
 * User account information: user list, user stats, selection.
 */

#ifndef PS3RECOMP_CELL_USERINFO_H
#define PS3RECOMP_CELL_USERINFO_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define CELL_USERINFO_USER_MAX              16
#define CELL_USERINFO_USERNAME_SIZE         64
#define CELL_USERINFO_HOME_PATH_SIZE        128

/* Default user */
#define CELL_USERINFO_DEFAULT_USER_ID       1

/* Error codes */
#define CELL_USERINFO_ERROR_BASE            0x8002B200
#define CELL_USERINFO_ERROR_BUSY            (CELL_USERINFO_ERROR_BASE | 0x01)
#define CELL_USERINFO_ERROR_INTERNAL        (CELL_USERINFO_ERROR_BASE | 0x02)
#define CELL_USERINFO_ERROR_PARAM           (CELL_USERINFO_ERROR_BASE | 0x03)
#define CELL_USERINFO_ERROR_NOUSER          (CELL_USERINFO_ERROR_BASE | 0x04)

/* User selection types */
#define CELL_USERINFO_LISTTYPE_ALL          0
#define CELL_USERINFO_LISTTYPE_SIGNIN       1

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/* User stat */
typedef struct CellUserInfoUserStat {
    u32  id;
    char name[CELL_USERINFO_USERNAME_SIZE];
    char homeDir[CELL_USERINFO_HOME_PATH_SIZE];
} CellUserInfoUserStat;

/* User list */
typedef struct CellUserInfoUserList {
    u32 userId[CELL_USERINFO_USER_MAX];
} CellUserInfoUserList;

/* Callback for user selection (simplified) */
typedef void (*CellUserInfoFinishCallback)(s32 result, u32 selectedUser, void* userdata);

/* User selection list set */
typedef struct CellUserInfoListSet {
    u32  focusUser;
    char title[128];
    u32  reserved;
} CellUserInfoListSet;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellUserInfoGetStat(u32 id, CellUserInfoUserStat* stat);
s32 cellUserInfoGetList(u32* listNum, CellUserInfoUserList* list, u32* currentUser);
s32 cellUserInfoSelectUser_ListSet(u32 listType,
                                    const CellUserInfoListSet* listSet,
                                    CellUserInfoFinishCallback callback,
                                    void* userdata);
s32 cellUserInfoSelectUser_ListGet(u32* selectedUser);
s32 cellUserInfoEnableOverlay(s32 enable);
s32 cellUserInfoGetHomeDir(u32 id, char* homePath, u32 homePathSize);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_USERINFO_H */

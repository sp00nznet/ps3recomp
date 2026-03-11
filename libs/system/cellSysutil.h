/*
 * ps3recomp - cellSysutil HLE stub
 *
 * Core system utility functions: callbacks, system parameters.
 */

#ifndef PS3RECOMP_CELL_SYSUTIL_H
#define PS3RECOMP_CELL_SYSUTIL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

/* System parameter IDs (cellSysutilGetSystemParamInt) */
#define CELL_SYSUTIL_SYSTEMPARAM_ID_LANG                 0x0111
#define CELL_SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN  0x0112
#define CELL_SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT          0x0114
#define CELL_SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT          0x0115
#define CELL_SYSUTIL_SYSTEMPARAM_ID_TIMEZONE             0x0116
#define CELL_SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME           0x0117
#define CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL  0x0121
#define CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL0_RESTRICT  0x0123
#define CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USER_HAS_NP_ACCOUNT    0x0141
#define CELL_SYSUTIL_SYSTEMPARAM_ID_CAMERA_PLFREQ        0x0151
#define CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_RUMBLE           0x0152
#define CELL_SYSUTIL_SYSTEMPARAM_ID_KEYBOARD_TYPE        0x0153
#define CELL_SYSUTIL_SYSTEMPARAM_ID_JAPANESE_KEYBOARD_ENTRY_METHOD  0x0154
#define CELL_SYSUTIL_SYSTEMPARAM_ID_CHINESE_KEYBOARD_ENTRY_METHOD  0x0155
#define CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_AUTOOFF          0x0156

/* System parameter string IDs (cellSysutilGetSystemParamString) */
#define CELL_SYSUTIL_SYSTEMPARAM_ID_NICKNAME             0x0113
#define CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME     0x0131

/* Language constants */
#define CELL_SYSUTIL_LANG_JAPANESE           0
#define CELL_SYSUTIL_LANG_ENGLISH_US         1
#define CELL_SYSUTIL_LANG_FRENCH             2
#define CELL_SYSUTIL_LANG_SPANISH            3
#define CELL_SYSUTIL_LANG_GERMAN             4
#define CELL_SYSUTIL_LANG_ITALIAN            5
#define CELL_SYSUTIL_LANG_DUTCH              6
#define CELL_SYSUTIL_LANG_PORTUGUESE_PT      7
#define CELL_SYSUTIL_LANG_RUSSIAN            8
#define CELL_SYSUTIL_LANG_KOREAN             9
#define CELL_SYSUTIL_LANG_CHINESE_T          10
#define CELL_SYSUTIL_LANG_CHINESE_S          11
#define CELL_SYSUTIL_LANG_FINNISH            12
#define CELL_SYSUTIL_LANG_SWEDISH            13
#define CELL_SYSUTIL_LANG_DANISH             14
#define CELL_SYSUTIL_LANG_NORWEGIAN          15
#define CELL_SYSUTIL_LANG_POLISH             16
#define CELL_SYSUTIL_LANG_PORTUGUESE_BR      17
#define CELL_SYSUTIL_LANG_ENGLISH_GB         18
#define CELL_SYSUTIL_LANG_TURKISH            19

/* Enter button assign */
#define CELL_SYSUTIL_ENTER_BUTTON_ASSIGN_CIRCLE  0
#define CELL_SYSUTIL_ENTER_BUTTON_ASSIGN_CROSS   1

/* Callback status codes */
#define CELL_SYSUTIL_REQUEST_EXITGAME         0x0101
#define CELL_SYSUTIL_DRAWING_BEGIN            0x0121
#define CELL_SYSUTIL_DRAWING_END              0x0122
#define CELL_SYSUTIL_SYSTEM_MENU_OPEN         0x0131
#define CELL_SYSUTIL_SYSTEM_MENU_CLOSE        0x0132
#define CELL_SYSUTIL_BGMPLAYBACK_PLAY         0x0141
#define CELL_SYSUTIL_BGMPLAYBACK_STOP         0x0142

/* Maximum callback slots */
#define CELL_SYSUTIL_MAX_CALLBACKS  4

/* Callback function type */
typedef void (*CellSysutilCallback)(u64 status, u64 param, void* userdata);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* NID: 0x9D848F34 */
s32 cellSysutilRegisterCallback(s32 slot, CellSysutilCallback func, void* userdata);

/* NID: 0x02FF3C1B */
s32 cellSysutilUnregisterCallback(s32 slot);

/* NID: 0x189A74DA */
s32 cellSysutilCheckCallback(void);

/* NID: 0x40E895D3 */
s32 cellSysutilGetSystemParamInt(s32 id, s32* value);

/* NID: 0x938013A0 */
s32 cellSysutilGetSystemParamString(s32 id, char* buf, u32 bufsize);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SYSUTIL_H */

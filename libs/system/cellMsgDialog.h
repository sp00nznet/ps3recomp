/*
 * ps3recomp - cellMsgDialog HLE
 *
 * System message dialog: yes/no, ok, progress bar.
 */

#ifndef PS3RECOMP_CELL_MSGDIALOG_H
#define PS3RECOMP_CELL_MSGDIALOG_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

/* Dialog type flags (bitfield packed into CellMsgDialogType) */
#define CELL_MSGDIALOG_TYPE_SE_TYPE_ERROR          0x00000000
#define CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL         0x00000001
#define CELL_MSGDIALOG_TYPE_SE_MUTE_ON             0x00000002
#define CELL_MSGDIALOG_TYPE_BG_VISIBLE             0x00000004
#define CELL_MSGDIALOG_TYPE_BG_INVISIBLE           0x00000000
#define CELL_MSGDIALOG_TYPE_BUTTON_TYPE_NONE       0x00000000
#define CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO      0x00000010
#define CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK         0x00000020
#define CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_ON      0x00000080
#define CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_OFF     0x00000000
#define CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_YES     0x00000000
#define CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_NO      0x00000100
#define CELL_MSGDIALOG_TYPE_PROGRESSBAR_NONE       0x00000000
#define CELL_MSGDIALOG_TYPE_PROGRESSBAR_SINGLE     0x00001000
#define CELL_MSGDIALOG_TYPE_PROGRESSBAR_DOUBLE     0x00002000

/* Button results */
#define CELL_MSGDIALOG_BUTTON_NONE     0
#define CELL_MSGDIALOG_BUTTON_INVALID  (-1)
#define CELL_MSGDIALOG_BUTTON_OK       1
#define CELL_MSGDIALOG_BUTTON_YES      1
#define CELL_MSGDIALOG_BUTTON_NO       2
#define CELL_MSGDIALOG_BUTTON_ESCAPE   3

/* Error codes */
#define CELL_MSGDIALOG_ERROR_PARAM         (s32)(CELL_ERROR_BASE_SYSUTIL | 0x01)
#define CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED (s32)(CELL_ERROR_BASE_SYSUTIL | 0x10)

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

typedef u32 CellMsgDialogType;

/* Callback: called when dialog closes */
typedef void (*CellMsgDialogCallback)(s32 buttonType, void* userdata);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellMsgDialogOpen2(CellMsgDialogType type, const char* msgString,
                        CellMsgDialogCallback callback, void* userdata,
                        void* extParam);

s32 cellMsgDialogClose(float delayMs);

s32 cellMsgDialogAbort(void);

s32 cellMsgDialogProgressBarSetMsg(u32 progressBarIndex, const char* msgString);

s32 cellMsgDialogProgressBarReset(u32 progressBarIndex);

s32 cellMsgDialogProgressBarInc(u32 progressBarIndex, u32 delta);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_MSGDIALOG_H */

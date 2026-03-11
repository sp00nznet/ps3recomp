/*
 * ps3recomp - cellMsgDialog HLE implementation
 *
 * Prints dialog messages to stdout and immediately invokes callbacks.
 * No actual UI is rendered.
 */

#include "cellMsgDialog.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int                   s_dialog_open = 0;
static CellMsgDialogCallback s_callback    = NULL;
static void*                 s_userdata    = NULL;
static CellMsgDialogType     s_type        = 0;

/* Progress bar state */
#define MAX_PROGRESS_BARS 2

typedef struct {
    u32  value;
    char message[256];
} ProgressBarState;

static ProgressBarState s_progress[MAX_PROGRESS_BARS];

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellMsgDialogOpen2(CellMsgDialogType type, const char* msgString,
                        CellMsgDialogCallback callback, void* userdata,
                        void* extParam)
{
    printf("[cellMsgDialog] Open2(type=0x%08X, msg='%s')\n",
           type, msgString ? msgString : "<null>");

    if (s_dialog_open) {
        printf("[cellMsgDialog] WARNING: dialog already open, closing previous\n");
    }

    s_dialog_open = 1;
    s_callback    = callback;
    s_userdata    = userdata;
    s_type        = type;
    memset(s_progress, 0, sizeof(s_progress));

    /* Print the message so developers can see it */
    if (msgString) {
        printf("========================================\n");
        printf("[DIALOG] %s\n", msgString);
        printf("========================================\n");
    }

    /* Determine button type and auto-respond */
    u32 button_type = type & 0x000000F0;
    int has_progress = (type & 0x0000F000) != 0;

    if (!has_progress) {
        /* Auto-respond immediately for non-progress dialogs */
        s32 result = CELL_MSGDIALOG_BUTTON_OK;

        if (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO) {
            result = CELL_MSGDIALOG_BUTTON_YES;
            printf("[cellMsgDialog] Auto-responding: YES\n");
        } else if (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK) {
            result = CELL_MSGDIALOG_BUTTON_OK;
            printf("[cellMsgDialog] Auto-responding: OK\n");
        } else {
            result = CELL_MSGDIALOG_BUTTON_NONE;
            printf("[cellMsgDialog] Auto-responding: NONE (no buttons)\n");
        }

        /* Close and invoke callback */
        s_dialog_open = 0;
        if (s_callback) {
            s_callback(result, s_userdata);
        }
    } else {
        printf("[cellMsgDialog] Progress bar dialog opened (will close on explicit Close/Abort)\n");
    }

    return CELL_OK;
}

s32 cellMsgDialogClose(float delayMs)
{
    printf("[cellMsgDialog] Close(delay=%.1f ms)\n", delayMs);

    if (!s_dialog_open) {
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    }

    s_dialog_open = 0;

    if (s_callback) {
        s_callback(CELL_MSGDIALOG_BUTTON_NONE, s_userdata);
        s_callback = NULL;
    }

    return CELL_OK;
}

s32 cellMsgDialogAbort(void)
{
    printf("[cellMsgDialog] Abort()\n");

    if (!s_dialog_open) {
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    }

    s_dialog_open = 0;

    if (s_callback) {
        s_callback(CELL_MSGDIALOG_BUTTON_ESCAPE, s_userdata);
        s_callback = NULL;
    }

    return CELL_OK;
}

s32 cellMsgDialogProgressBarSetMsg(u32 progressBarIndex, const char* msgString)
{
    if (progressBarIndex >= MAX_PROGRESS_BARS)
        return CELL_MSGDIALOG_ERROR_PARAM;

    if (!s_dialog_open)
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;

    if (msgString) {
        strncpy(s_progress[progressBarIndex].message, msgString,
                sizeof(s_progress[progressBarIndex].message) - 1);
        s_progress[progressBarIndex].message[sizeof(s_progress[progressBarIndex].message) - 1] = '\0';
        printf("[cellMsgDialog] ProgressBar[%u] msg='%s'\n", progressBarIndex, msgString);
    }

    return CELL_OK;
}

s32 cellMsgDialogProgressBarReset(u32 progressBarIndex)
{
    if (progressBarIndex >= MAX_PROGRESS_BARS)
        return CELL_MSGDIALOG_ERROR_PARAM;

    if (!s_dialog_open)
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;

    s_progress[progressBarIndex].value = 0;
    printf("[cellMsgDialog] ProgressBar[%u] reset to 0%%\n", progressBarIndex);

    return CELL_OK;
}

s32 cellMsgDialogProgressBarInc(u32 progressBarIndex, u32 delta)
{
    if (progressBarIndex >= MAX_PROGRESS_BARS)
        return CELL_MSGDIALOG_ERROR_PARAM;

    if (!s_dialog_open)
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;

    s_progress[progressBarIndex].value += delta;
    if (s_progress[progressBarIndex].value > 100)
        s_progress[progressBarIndex].value = 100;

    printf("[cellMsgDialog] ProgressBar[%u] = %u%%\n",
           progressBarIndex, s_progress[progressBarIndex].value);

    return CELL_OK;
}

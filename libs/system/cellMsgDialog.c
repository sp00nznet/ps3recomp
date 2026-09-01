/*
 * ps3recomp - cellMsgDialog HLE implementation
 *
 * Prints dialog messages to stdout and immediately invokes callbacks.
 * No actual UI is rendered.
 */

#include "cellMsgDialog.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_read/vm_write: guest EA -> host */

/* Pointer parameters here are GUEST addresses, and the dialog callback is a
 * guest OPD -- the HLE ABI adapter passes both straight through as guest
 * values. Printing msgString as a host char* faulted the moment Tokyo Jungle
 * opened its data-install prompt, and calling s_callback as a host function
 * pointer would jump into the middle of guest memory. */
extern uint8_t* vm_base;

static const char* guest_str(const void* p)
{
    uint32_t ea = (uint32_t)(uintptr_t)p;
    return (ea && vm_base) ? (const char*)(vm_base + ea) : "<null>";
}

/* The callback is a guest OPD; dispatch it the way every other guest callback
 * in the runtime is dispatched. */
static void invoke_dialog_callback(CellMsgDialogCallback cb, int32_t result,
                                   void* userdata)
{
    uint32_t opd = (uint32_t)(uintptr_t)cb;
    if (!opd || !g_ps3_guest_caller) return;
    g_ps3_guest_caller(opd, (uint64_t)(int64_t)result,
                       (uint64_t)(uintptr_t)userdata, 0, 0, 0, 0, 0, 0);
}

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
           type, guest_str(msgString));

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
        printf("[DIALOG] %s\n", guest_str(msgString));
        printf("========================================\n");
    }

    /* Determine button type and auto-respond */
    u32 button_type = type & 0x000000F0;
    int has_progress = (type & 0x0000F000) != 0;

    if (!has_progress) {
        /* Auto-respond immediately for non-progress dialogs */
        s32 result = CELL_MSGDIALOG_BUTTON_OK;

        if (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO) {
            /* MSGDIALOG_ANSWER=no answers every yes/no prompt NO instead. The
             * auto-answer is a guess about what the title wants, and yes is not
             * always the boot-friendliest one: a "use game data?" prompt
             * answered yes sends the title down an install/cache path a port may
             * have nothing behind, where no just plays from disc. A knob costs
             * less than a rebuild to try the other branch. */
            static int no_ = -1;
            if (no_ < 0) { const char* e = getenv("MSGDIALOG_ANSWER");
                           no_ = (e && (e[0] == 'n' || e[0] == 'N')) ? 1 : 0; }
            result = no_ ? CELL_MSGDIALOG_BUTTON_NO : CELL_MSGDIALOG_BUTTON_YES;
            printf("[cellMsgDialog] Auto-responding: %s\n", no_ ? "NO" : "YES");
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
            invoke_dialog_callback(s_callback, result, s_userdata);
        }
    } else {
        printf("[cellMsgDialog] Progress bar dialog opened (will close on explicit Close/Abort)\n");
    }

    return CELL_OK;
}

/* cellMsgDialogOpen -- the pre-3.40 entry point, identical arguments and
 * behaviour to Open2 (RPCS3 forwards it the same way). Registering only Open2
 * meant a title on the older API got the unresolved-NID default and then waited
 * forever for a callback that could never fire. Virtua Fighter 5 opens one
 * during boot and stops dead there. */
s32 cellMsgDialogOpen(CellMsgDialogType type, const char* msgString,
                      CellMsgDialogCallback callback, void* userdata,
                      void* extParam)
{
    return cellMsgDialogOpen2(type, msgString, callback, userdata, extParam);
}

s32 cellMsgDialogClose(float delayMs)
{
    printf("[cellMsgDialog] Close(delay=%.1f ms)\n", delayMs);

    if (!s_dialog_open) {
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    }

    s_dialog_open = 0;

    if (s_callback) {
        invoke_dialog_callback(s_callback, CELL_MSGDIALOG_BUTTON_NONE, s_userdata);
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
        invoke_dialog_callback(s_callback, CELL_MSGDIALOG_BUTTON_ESCAPE, s_userdata);
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
        strncpy(s_progress[progressBarIndex].message, GUEST_PTR(msgString, const char*),
                sizeof(s_progress[progressBarIndex].message) - 1);
        s_progress[progressBarIndex].message[sizeof(s_progress[progressBarIndex].message) - 1] = '\0';
        printf("[cellMsgDialog] ProgressBar[%u] msg='%s'\n", progressBarIndex, guest_str(msgString));
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

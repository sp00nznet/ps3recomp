/*
 * ps3recomp - cellMsgDialog HLE implementation
 *
 * Prints dialog messages to stdout and queues callbacks for sysutil polling.
 * No actual UI is rendered.
 */

#include "cellMsgDialog.h"
#include "cellSysutil.h"
#include "ps3emu/guest_call.h"
#include "ps3emu/strict_boot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>   /* tolower: the yes/no auto-answer heuristic */
#include <math.h>
#include "../../runtime/platform/win32_compat.h"   /* GetTickCount64 on POSIX too */
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_read/vm_write: guest EA -> host */

/* Pointer parameters here are GUEST addresses, and the dialog callback is a
 * guest OPD -- the HLE ABI adapter passes both straight through as guest
 * values. Printing msgString as a host char* faulted the moment Tokyo Jungle
 * opened its data-install prompt, and calling s_callback as a host function
 * pointer would jump into the middle of guest memory. */
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;

#ifdef _WIN32
static SRWLOCK s_dialog_lock = SRWLOCK_INIT;
#define DIALOG_LOCK() AcquireSRWLockExclusive(&s_dialog_lock)
#define DIALOG_UNLOCK() ReleaseSRWLockExclusive(&s_dialog_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_dialog_lock = PTHREAD_MUTEX_INITIALIZER;
#define DIALOG_LOCK() pthread_mutex_lock(&s_dialog_lock)
#define DIALOG_UNLOCK() pthread_mutex_unlock(&s_dialog_lock)
#endif

static const char* guest_str(const void* p)
{
    uint32_t ea = (uint32_t)(uintptr_t)p;
    return (ea && vm_base) ? (const char*)(vm_base + ea) : "<null>";
}

static int dialog_copy_string(const void* p,char* output,size_t capacity)
{
    u32 ea=(u32)(uintptr_t)p;
    const uint64_t limit=ppu_vm_size?ppu_vm_size:0x100000000ull;
    size_t available=ea<limit?(size_t)((limit-ea)<capacity?limit-ea:capacity):0;
    if(!ea || !vm_base || !available)return 0;
    const char* source=(const char*)vm_base+ea;
    const char* end=memchr(source,0,available);
    if(!end)return 0;
    size_t length=(size_t)(end-source);
    memcpy(output,source,length);output[length]=0;return 1;
}

/* Do not run a completion before Open2 returns: games establish their
 * dialog wait state after opening, then consume the result on a sysutil poll. */
static s32 queue_dialog_callback(CellMsgDialogCallback cb, int32_t result,
                                 void* userdata)
{
    return cellSysutilQueueGuestCallback((u32)(uintptr_t)cb,
        (u64)(int64_t)result, (u64)(uintptr_t)userdata);
}

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int                   s_dialog_open = 0;
static CellMsgDialogCallback s_callback    = NULL;
static void*                 s_userdata    = NULL;
static CellMsgDialogType     s_type        = 0;
static uint64_t              s_close_deadline;
static int                  s_close_pending;

/* Deadline for auto-dismissing a no-button dialog; 0 = not armed.
 *
 * A BUTTON_TYPE_NONE dialog waits for the title to take it down (see
 * cellMsgDialogOpen2), which is right for a progress notice the title closes
 * itself -- Virtua Fighter 5 Aborts its "Checking game data" notice when the
 * check finishes. It is wrong for the notice at the END of that check:
 * "Check complete." is a no-button dialog the title expects a PERSON to
 * dismiss, and with nobody there the title waits on it forever.
 *
 * Both arrive as type 0x01, so nothing in the type distinguishes them, and
 * guessing from the message text would be the same fragile trick the yes/no
 * answer already has to use. A grace period needs no guess: the title gets
 * first refusal on closing its own dialog, and only one it has left up beyond
 * the deadline is dismissed for it. MSGDIALOG_AUTODISMISS_MS tunes it; 0 waits
 * forever, which is the behaviour to pick if a title turns out to hold a
 * legitimate notice open longer than the default. */
static ULONGLONG s_nobutton_deadline = 0;



static unsigned nobutton_grace_ms(void)
{
    static int ms = -1;
    if (ms < 0) { const char* e = getenv("MSGDIALOG_AUTODISMISS_MS");
                  ms = e ? atoi(e) : 2000; if (ms < 0) ms = 0; }
    return (unsigned)ms;
}


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

/* Does this yes/no prompt offer to ABORT something the title is doing?
 * Case-insensitive substring match on the words a title uses for it. Only
 * consulted when MSGDIALOG_ANSWER is unset; the call site explains why the
 * dialog's own type field cannot answer this. */
static int msg_offers_to_abort(const char* msg)
{
    static const char* const words[] = { "cancel", "quit", "abort", NULL };
    if (!msg) return 0;
    for (int w = 0; words[w]; w++) {
        const size_t n = strlen(words[w]);
        for (const char* p = msg; *p; p++) {
            size_t i = 0;
            while (i < n && p[i] && (char)tolower((unsigned char)p[i]) == words[w][i]) i++;
            if (i == n) return 1;
        }
    }
    return 0;
}

static s32 dialog_open_impl(CellMsgDialogType type, const char* msgString,
                        CellMsgDialogCallback callback, void* userdata,
                        void* extParam)
{
    (void)extParam;
    const int strict = ps3_strict_boot_enabled();
    char bounded_message[512];
    if (strict) {
        u32 buttons=(type>>4)&7u, cursor=(type>>8)&3u, bars=(type>>12)&3u;
        if (!dialog_copy_string(msgString,bounded_message,sizeof(bounded_message)) ||
            (type&~0x33F7u) || buttons>2u ||
            (buttons==0 && (cursor || bars>2)) ||
            (buttons==1 && (cursor>1 || bars)) ||
            (buttons==2 && (cursor || bars))) return CELL_MSGDIALOG_ERROR_PARAM;
        if (s_dialog_open) return (s32)0x8002B105u;
        if (buttons) {
            ps3_strict_boot_guard("msgdialog-user-input-unimplemented","type=0x%X",type);
            return (s32)CELL_ENOSYS;
        }
        s32 queued=cellSysutilSendSystemCommand(CELL_SYSUTIL_DRAWING_BEGIN,0);
        if (queued<0) return queued;
    }
    const char* display_message=strict?bounded_message:guest_str(msgString);
    printf("[cellMsgDialog] Open2(type=0x%08X, msg='%s')\n",type,display_message);

    if (s_dialog_open) {
        printf("[cellMsgDialog] WARNING: dialog already open, closing previous\n");
    }

    s_dialog_open = 1;
    s_callback    = callback;
    s_userdata    = userdata;
    s_type        = type;
    s_close_pending=0;
    memset(s_progress, 0, sizeof(s_progress));

    /* Print the message so developers can see it */
    if (msgString) {
        printf("========================================\n");
        printf("[DIALOG] %s\n", display_message);
        printf("========================================\n");
    }

    /* Determine button type and auto-respond */
    u32 button_type = type & 0x00000030;
    int has_progress = (type & 0x0000F000) != 0;

    /* A BUTTON_TYPE_NONE dialog is an informational notice with no buttons: on
     * hardware it stays on screen until the TITLE takes it down with
     * cellMsgDialogClose/Abort, and only then does its callback fire. We used
     * to queue that callback immediately, which told the title its own notice
     * had already been dismissed.
     *
     * Virtua Fighter 5 shows what that costs. It puts up "Checking game data.
     * This may take several minutes" (type 0x01 -- SE_NORMAL, BUTTON_TYPE_NONE),
     * we instantly reported it closed, the title read that as the player
     * backing out and asked "Are you sure you want to cancel checking the game
     * data?", and on being told no it reopened the notice -- 26 times in a
     * 260-second boot, which is where the boot time was going.
     *
     * So a no-button dialog waits, exactly like the progress bar already did.
     * Close() still queues BUTTON_NONE and Abort() still queues BUTTON_ESCAPE,
     * so the title gets its callback -- when it asks for it. */
    /* OFF BY DEFAULT, and that is a deliberate retreat rather than a doubt
     * about the diagnosis. Holding the no-button dialog open is what hardware
     * does and it removes VF5's check loop completely (26 reopenings -> 1).
     * It then stalls the title HARDER than the bug did: it stops flipping
     * altogether at ~3,300 frames, where the old behaviour looped noisily but
     * still reached its logo sequence. Dismissing the leftover "Check
     * complete." after a grace period does not help, as NONE or as ESCAPE, and
     * neither does absorbing the title's own Close/Abort afterwards -- all
     * three were built and measured. So there is a third thing VF5 needs after
     * its game-data check that we do not do, and the loop was hiding it.
     *
     * Correct-but-hangs is worse than wrong-but-boots for a title people are
     * trying to run, so this waits behind MSGDIALOG_NOBUTTON_WAITS=1 until the
     * stall behind it is understood. */
    static int nobutton_waits = -1;
    if (nobutton_waits < 0) nobutton_waits = getenv("MSGDIALOG_NOBUTTON_WAITS") ? 1 : 0;
    int stays_open = strict || has_progress ||
                     (nobutton_waits &&
                      button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_NONE);

    if (!stays_open) {
        /* Queue the headless auto-response for non-progress dialogs */
        s32 result = CELL_MSGDIALOG_BUTTON_OK;

        if (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO) {
            /* MSGDIALOG_ANSWER=no answers every yes/no prompt NO instead. The
             * auto-answer is a guess about what the title wants, and yes is not
             * always the boot-friendliest one: a "use game data?" prompt
             * answered yes sends the title down an install/cache path a port may
             * have nothing behind, where no just plays from disc. A knob costs
             * less than a rebuild to try the other branch. */
            /* MSGDIALOG_ANSWER=auto answers whichever button lets the title
             * CONTINUE -- NO for a prompt that offers to abort something, YES
             * otherwise:
             *
             *   "Do you want to use game data?"               -> YES
             *   "Are you sure you want to cancel checking the
             *    game data?"                                  -> NO
             *   "Do you want to cancel the load operation?"   -> NO
             *
             * That is what a person would answer, and it stops the runtime
             * cancelling a title's own work. It is NOT the default, and that is
             * a measurement rather than a preference.
             *
             * Virtua Fighter 5 asks the last two during its boot. Answered YES
             * it cancels its own game-data check and then its own load, which
             * sounds unambiguously bad -- but cancelling the check is how that
             * title gets ON with its boot here, and answering NO instead leaves
             * it reopening the check forever. Over five minutes, =auto executes
             * 0 draw groups; the default YES executes 12,829 and reaches
             * cellSaveDataFixedLoad2. So the destructive answer is currently
             * the one that works, which means something downstream of a
             * COMPLETED check is missing, and being right about the button is
             * worth less than the title booting until that is found.
             *
             * The dialog TYPE cannot decide this either way: both VF5 prompts
             * arrive as 0x11 with the default cursor on YES, because on
             * hardware the cursor only says where the highlight starts and a
             * person reads the sentence. So =auto keys on the text, English
             * words only, and a localised build falls through to YES. */
            static int forced = -1;   /* 0 = force YES, 1 = force NO, 2 = auto */
            if (forced < 0) {
                const char* e = getenv("MSGDIALOG_ANSWER");
                if      (e && (e[0] == 'n' || e[0] == 'N')) forced = 1;   /* always NO */
                else if (e && (e[0] == 'a' || e[0] == 'A')) forced = 2;   /* heuristic */
                else                                        forced = 0;   /* always YES (default) */
            }
            int auto_no = (forced == 2) ? msg_offers_to_abort(guest_str(msgString)) : 0;
            int no_ = (forced == 2) ? auto_no : forced;
            result = no_ ? CELL_MSGDIALOG_BUTTON_NO : CELL_MSGDIALOG_BUTTON_YES;
            printf("[cellMsgDialog] Auto-responding: %s%s\n", no_ ? "NO" : "YES",
                   auto_no ? " (prompt offers to abort)" : "");
        } else if (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK) {
            result = CELL_MSGDIALOG_BUTTON_OK;
            printf("[cellMsgDialog] Auto-responding: OK\n");
        } else {
            result = CELL_MSGDIALOG_BUTTON_NONE;
            printf("[cellMsgDialog] Auto-responding: NONE (no buttons)\n");
        }

        /* Close and queue the callback; never enter guest code here. */
        s_dialog_open = 0;
        if (s_callback) {
            CellMsgDialogCallback cb = s_callback;
            s_callback = NULL;
            return queue_dialog_callback(cb, result, s_userdata);
        }
    } else {
        printf("[cellMsgDialog] %s dialog opened (waits for Close/Abort)\n",
               has_progress ? "Progress bar" : "No-button");
        s_nobutton_deadline = (!strict && !has_progress && nobutton_grace_ms())
            ? GetTickCount64() + nobutton_grace_ms() : 0;
    }

    return CELL_OK;
}

s32 cellMsgDialogOpen2(CellMsgDialogType type,const char* msgString,
                       CellMsgDialogCallback callback,void* userdata,void* extParam)
{
    DIALOG_LOCK();
    s32 result=dialog_open_impl(type,msgString,callback,userdata,extParam);
    DIALOG_UNLOCK();return result;
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

static s32 finish_strict_dialog(int callback)
{
    s32 queued=cellSysutilSendSystemCommand(CELL_SYSUTIL_DRAWING_END,0);
    if (queued<0) {
        ps3_strict_boot_guard("msgdialog-end-allocation-failed","error=0x%X",(u32)queued);
        return queued;
    }
    s_dialog_open=0;s_nobutton_deadline=0;s_close_pending=0;
    CellMsgDialogCallback cb=s_callback;s_callback=NULL;
    if (callback && cb) {
        s32 result=queue_dialog_callback(cb,CELL_MSGDIALOG_BUTTON_NONE,s_userdata);
        if (result<0) ps3_strict_boot_guard("msgdialog-completion-allocation-failed","error=0x%X",(u32)result);
        return result;
    }
    return CELL_OK;
}

static s32 dialog_close_impl(float delayMs)
{
    printf("[cellMsgDialog] Close(delay=%.1f ms)\n", delayMs);

    if (!s_dialog_open) {
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    }
    if (ps3_strict_boot_enabled()) {
        if (!isfinite(delayMs)) return CELL_MSGDIALOG_ERROR_PARAM;
        if (delayMs<=0) return finish_strict_dialog(1);
        double milliseconds=ceil((double)delayMs);
        uint64_t now=GetTickCount64();
        if (milliseconds>=0x1p64 || milliseconds>(double)(UINT64_MAX-now)) return CELL_MSGDIALOG_ERROR_PARAM;
        s_close_deadline=now+(uint64_t)milliseconds;s_close_pending=1;
        return CELL_OK;
    }

    s_dialog_open = 0;
    s_nobutton_deadline = 0;

    if (s_callback) {
        CellMsgDialogCallback cb = s_callback;
        s_callback = NULL;
        return queue_dialog_callback(cb, CELL_MSGDIALOG_BUTTON_NONE, s_userdata);
    }

    return CELL_OK;
}

s32 cellMsgDialogClose(float delayMs)
{
    DIALOG_LOCK();s32 result=dialog_close_impl(delayMs);DIALOG_UNLOCK();return result;
}

static s32 dialog_abort_impl(void)
{
    printf("[cellMsgDialog] Abort()\n");

    if (ps3_strict_boot_enabled()) return s_dialog_open?finish_strict_dialog(0):CELL_OK;
    if (!s_dialog_open) {
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    }

    s_dialog_open = 0;
    s_nobutton_deadline = 0;

    if (s_callback) {
        CellMsgDialogCallback cb = s_callback;
        s_callback = NULL;
        return queue_dialog_callback(cb, CELL_MSGDIALOG_BUTTON_ESCAPE, s_userdata);
    }

    return CELL_OK;
}

s32 cellMsgDialogAbort(void)
{
    DIALOG_LOCK();s32 result=dialog_abort_impl();DIALOG_UNLOCK();return result;
}

static s32 dialog_progress_message_impl(u32 progressBarIndex, const char* msgString)
{
    char bounded[64];
    const int strict=ps3_strict_boot_enabled();
    if(strict && !dialog_copy_string(msgString,bounded,sizeof(bounded)))return CELL_MSGDIALOG_ERROR_PARAM;
    if (progressBarIndex >= MAX_PROGRESS_BARS)
        return CELL_MSGDIALOG_ERROR_PARAM;
    if (!s_dialog_open)
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    if(ps3_strict_boot_enabled() && progressBarIndex>=((s_type>>12)&3u))
        return CELL_MSGDIALOG_ERROR_PARAM;

    if (msgString) {
        const char* message=strict?bounded:GUEST_PTR(msgString,const char*);
        strncpy(s_progress[progressBarIndex].message, message,
                sizeof(s_progress[progressBarIndex].message) - 1);
        s_progress[progressBarIndex].message[sizeof(s_progress[progressBarIndex].message) - 1] = '\0';
        printf("[cellMsgDialog] ProgressBar[%u] msg='%s'\n", progressBarIndex, message);
    }

    return CELL_OK;
}

static s32 dialog_progress_reset_impl(u32 progressBarIndex)
{
    if (progressBarIndex >= MAX_PROGRESS_BARS)
        return CELL_MSGDIALOG_ERROR_PARAM;
    if (!s_dialog_open)
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    if(ps3_strict_boot_enabled() && progressBarIndex>=((s_type>>12)&3u))
        return CELL_MSGDIALOG_ERROR_PARAM;

    s_progress[progressBarIndex].value = 0;
    printf("[cellMsgDialog] ProgressBar[%u] reset to 0%%\n", progressBarIndex);

    return CELL_OK;
}

static s32 dialog_progress_inc_impl(u32 progressBarIndex, u32 delta)
{
    if (progressBarIndex >= MAX_PROGRESS_BARS)
        return CELL_MSGDIALOG_ERROR_PARAM;
    if (!s_dialog_open)
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    if(ps3_strict_boot_enabled() && progressBarIndex>=((s_type>>12)&3u))
        return CELL_MSGDIALOG_ERROR_PARAM;

    uint64_t sum=(uint64_t)s_progress[progressBarIndex].value+delta;
    s_progress[progressBarIndex].value=sum>100?100:(u32)sum;

    printf("[cellMsgDialog] ProgressBar[%u] = %u%%\n",
           progressBarIndex, s_progress[progressBarIndex].value);

    return CELL_OK;
}

s32 cellMsgDialogProgressBarSetMsg(u32 index,const char* message)
{
    DIALOG_LOCK();s32 result=dialog_progress_message_impl(index,message);DIALOG_UNLOCK();return result;
}
s32 cellMsgDialogProgressBarReset(u32 index)
{
    DIALOG_LOCK();s32 result=dialog_progress_reset_impl(index);DIALOG_UNLOCK();return result;
}
s32 cellMsgDialogProgressBarInc(u32 index,u32 delta)
{
    DIALOG_LOCK();s32 result=dialog_progress_inc_impl(index,delta);DIALOG_UNLOCK();return result;
}


/* Called from cellSysutilCheckCallback each time the title polls sysutil.
 * Dismisses a no-button dialog the title has left open past its grace period;
 * see s_nobutton_deadline for why this cannot be decided at open time. */
static void dialog_tick_impl(void)
{
    if (ps3_strict_boot_enabled()) {
        if (s_dialog_open && s_close_pending && GetTickCount64()>=s_close_deadline)
            (void)finish_strict_dialog(1);
        return;
    }
    if (!s_dialog_open || !s_nobutton_deadline) return;
    if (GetTickCount64() < s_nobutton_deadline) return;
    s_nobutton_deadline = 0;
    s_dialog_open = 0;
    printf("[cellMsgDialog] no-button dialog auto-dismissed after %u ms\n",
           nobutton_grace_ms());
    if (s_callback) {
        CellMsgDialogCallback cb = s_callback;
        s_callback = NULL;
        queue_dialog_callback(cb, CELL_MSGDIALOG_BUTTON_NONE, s_userdata);
    }
}

void cellMsgDialog_tick(void)
{
    DIALOG_LOCK();dialog_tick_impl();DIALOG_UNLOCK();
}

# Native on-screen keyboard (cellOskDialog)

By default `cellOskDialog` is a stub that answers with an empty or configured
string. A build can instead route the dialog to a native host, so that a user
answers a real prompt and the title sees the normal lifecycle. Hosts are opt-in
at configure time; without one nothing changes.

| CMake option | Platforms | Host |
|---|---|---|
| `PS3RECOMP_NATIVE_OSK_HEADLESS` | POSIX other than macOS | No window: the dialog opens with its initial text and is confirmed only through the control mailbox (below). |
| `PS3RECOMP_NATIVE_OSK_WINDOWS` | Windows | A small dialog owned by the game window of the null/live or D3D12 backend (below). |

## Lifecycle

`libs/misc/osk_native.c` owns the guest-visible state; a host owns only its
user interface.

- `cellOskDialogLoadAsync` copies the prompt, initial text and limits out of
  guest memory and hands the copy to the host with a new generation token. It
  does not require `cellOskDialogInit`. A second load while one is active
  returns `CELL_SYSUTIL_ERROR_BUSY`; an unavailable host returns an error rather
  than invented text.
- The host reports "opened" and later OK or Cancel with host-owned UTF-16 text.
  Those calls only set event bits. The title's own `cellSysutilCheckCallback`
  turns them into `CELL_SYSUTIL_OSKDIALOG_LOADED` (0x502), `FINISHED` (0x503)
  and, after `cellOskDialogUnloadAsync`, `UNLOADED` (0x504). Late reports for an
  older token are ignored.
- `cellOskDialogUnloadAsync` writes the result and big-endian UTF-16 text,
  bounded by the caller's capacity plus terminator, without splitting a
  surrogate pair. `cellOskDialogAbort` finishes with the abort result.
- The prohibit flags for spaces and line breaks are applied to the result.
  Password panels are never prefilled, and prompts and input are not logged.

## Unattended runs

- `PS3RECOMP_OSK_INITIAL_TEXT` (UTF-8) prefills the first non-password dialog of
  the process. An invalid UTF-8 value is ignored as a whole.
- `OSK_NATIVE_CONTROL_FILE` names a mailbox, read once at the first request.
  When a dialog has opened, the runtime prints
  `[cellOskDialog] native control ready token=<n>`; then
  `python3 tools/native_osk_confirm.py <path> <n>` publishes `confirm <n>`
  atomically, which presses the host's own OK for that generation at most once.
  Every file found is consumed, valid or not; commands written before the
  dialog opened, for another token, or repeated are ignored.

## Windows host

With `PS3RECOMP_NATIVE_OSK_WINDOWS=ON` the null/live and D3D12 backends
register their game window, and a request shows as a small window owned by it:
the prompt, an edit control with the initial text (masked for password panels,
read-only for a zero limit), OK and Cancel. The dialog is created, fed and
destroyed only on the thread that owns the game window, from that backend's
message pump, which then reads messages as Unicode and routes the dialog's
keyboard messages to it. Other threads only queue copied requests, closes and
confirmations, and wake that thread. Closing the game window closes the dialog
and makes the host unavailable. The dialog takes focus only when the game
window was in the foreground.

While a dialog is queued or open, until the title unloads it, it owns the
keyboard: the keyboard pad fallback in `cellPad` reports nothing, and a key
still held when it closes is ignored until it is released.
`OSK_NATIVE_CONTROL_FILE` and `PS3RECOMP_OSK_INITIAL_TEXT` are read as UTF-16
from the environment.

## Tests

- `python3 tools/test_native_osk.py` builds the real `cellOskDialog.c`,
  `osk_native.c` and `cellSysutil.c` against a simulated host and checks the
  lifecycle, Unicode limits, cancel/abort, stale tokens, guest-buffer bounds,
  the initial-text override and the mailbox.
- `python3 tools/test_native_osk_headless.py` runs the headless host end to end.
- `python3 tools/test_native_osk_control.py` checks the confirm helper.
- `python tools/test_native_osk_windows.py --runtime-library <ps3recomp_runtime.lib>`
  (Windows; a `/MT` runtime built with `PS3RECOMP_NATIVE_OSK_WINDOWS=ON`) drives
  real NID dispatch and the real dialog controls over a real parent window on
  a private desktop: Unicode prompt, prefill and typed text, limits, secure
  Cancel, read-only zero limit, abort before and after opening, closes that
  race the owner thread, stale requests, a worker-thread caller, the control
  mailbox, both backends' pumps and closing the parent.

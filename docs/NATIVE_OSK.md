# Native on-screen keyboard (cellOskDialog)

By default `cellOskDialog` is a stub that answers with an empty or configured
string. A build can instead route the dialog to a native host, so that a user
answers a real prompt and the title sees the normal lifecycle. Hosts are opt-in
at configure time; without one nothing changes.

| CMake option | Platforms | Host |
|---|---|---|
| `PS3RECOMP_NATIVE_OSK_HEADLESS` | POSIX other than macOS | No window: the dialog opens with its initial text and is confirmed only through the control mailbox (below). |
| `PS3RECOMP_NATIVE_OSK_MACOS` | macOS | An AppKit sheet on the Metal backend's game window (below). |

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

## macOS host

With `PS3RECOMP_NATIVE_OSK_MACOS=ON` the Metal backend registers its game
window, and a request shows as a sheet on it: the prompt, a text field with the
initial text (a secure field for password panels), OK and Cancel. All AppKit
work happens on the main thread; the guest side only sees the copied result
through the normal lifecycle. Closing the game window cancels an open sheet
and makes the host unavailable.

While a sheet is open it owns the keyboard: the backend's event pump hands key
events to AppKit (it otherwise drops them), and the keyboard pad fallback in
`cellPad` reports nothing; a key still held when the sheet closes (the Return
that pressed OK) is ignored until it is released. After OK or Cancel the game
window gets keyboard focus back on the next main-queue turn, unless something
else took it meanwhile: the user picked another window or application, another
sheet is up, a newer dialog opened, or the dialog was aborted or the window
replaced. The host never activates the application.

## Tests

- `python3 tools/test_native_osk.py` builds the real `cellOskDialog.c`,
  `osk_native.c` and `cellSysutil.c` against a simulated host and checks the
  lifecycle, Unicode limits, cancel/abort, stale tokens, guest-buffer bounds,
  the initial-text override and the mailbox.
- `python3 tools/test_native_osk_headless.py` runs the headless host end to end.
- `python3 tools/test_native_osk_control.py` checks the confirm helper.
- `python3 tools/test_native_osk_macos.py` (macOS, interactive graphical
  session; it takes keyboard focus for a few seconds and skips under `CI`)
  launches a real AppKit fixture as an app bundle: Unicode OK, secure Cancel,
  typed keys reaching the sheet and not the game view, the control mailbox,
  the focus hand-back guards (another window, a follow-up sheet, a replaced
  window, another application, abort) and parent close.

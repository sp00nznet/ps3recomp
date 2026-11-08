/*
 * ps3recomp - native on-screen keyboard (cellOskDialog) host bridge
 *
 * cellOskDialog.c routes LoadAsync/UnloadAsync/GetInputText/Abort here when a
 * native host is built in. osk_native.c owns the guest-visible lifecycle (one
 * dialog at a time, a generation token per request, LOADED/FINISHED/UNLOADED
 * delivered from the guest's own cellSysutilCheckCallback poll). A host owns
 * only the user interface: it receives a copied request and reports back with
 * host-owned data. No guest address crosses into a host.
 *
 * Hosts are opt-in at build time:
 *   PS3RECOMP_NATIVE_OSK_HEADLESS  POSIX without a toolkit dialog; a dialog is
 *                                  confirmed only through the control mailbox
 *                                  (osk_headless.c).
 *   PS3RECOMP_NATIVE_OSK_SDL       POSIX other than macOS: an SDL2 dialog owned
 *                                  by the thread that pumps the game window
 *                                  (osk_sdl.c). With the headless host also
 *                                  built, requests made while no window is
 *                                  registered go to it.
 * PS3RECOMP_DISABLE_NATIVE_OSK forces the legacy stub. PS3RECOMP_TEST_NATIVE_OSK
 * builds the lifecycle for a test that supplies its own host.
 */
#ifndef PS3RECOMP_OSK_NATIVE_H
#define PS3RECOMP_OSK_NATIVE_H
#include <stdint.h>

#if (!defined(_WIN32) && !defined(__APPLE__) && \
     (defined(PS3RECOMP_NATIVE_OSK_HEADLESS) || defined(PS3RECOMP_NATIVE_OSK_SDL)) && \
     !defined(PS3RECOMP_DISABLE_NATIVE_OSK)) || defined(PS3RECOMP_TEST_NATIVE_OSK)
#define PS3RECOMP_NATIVE_OSK 1
#endif
/* The SDL host owns the host entry points; the headless host becomes its
 * no-window fallback under the osk_headless_* names. */
#if !defined(_WIN32) && !defined(__APPLE__) && defined(PS3RECOMP_NATIVE_OSK_SDL) && \
    !defined(PS3RECOMP_DISABLE_NATIVE_OSK) && !defined(PS3RECOMP_TEST_NATIVE_OSK)
#define OSK_NATIVE_HAS_SDL_HOST 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A request copied out of guest memory. Strings are host-endian UTF-16. */
typedef struct {
    uint64_t token;                  /* generation; never reused in a process */
    uint32_t limit, panels, prohibit;
    uint16_t message[513], initial[513];
} OskNativeRequest;

/* ---- implemented by the host ---------------------------------------------- */
int  osk_native_available(void);
void osk_native_open(const OskNativeRequest* request);   /* any thread */
void osk_native_close(uint64_t token);                   /* any thread */
/* Press the host's own OK for an open generation (control mailbox). */
void osk_native_confirm(uint64_t token);

/* ---- implemented by osk_native.c, called by the host ---------------------- */
int  osk_native_is_pending(uint64_t token);   /* loaded, not yet shown */
int  osk_native_is_open(uint64_t token);
void osk_native_did_open(uint64_t token);
/* result: 0 OK (text applies), 1 cancelled by the user. */
void osk_native_did_finish(uint64_t token, int32_t result, const uint16_t* text, uint32_t length);
/* The host went away (window closed) before the user answered. */
void osk_native_host_closed(uint64_t token);
/* True only for the current generation with a user OK/Cancel result. */
int  osk_native_has_ui_result(uint64_t token);

/* ---- called by cellOskDialog.c with guest addresses ------------------------ */
int32_t osk_native_load(uint32_t param, uint32_t input);
int32_t osk_native_unload(uint32_t result);
int32_t osk_native_get_text(uint32_t result);
int32_t osk_native_abort(void);
/* Delivers pending lifecycle events; run from cellSysutilCheckCallback. */
void cellOskDialog_tick(void);

#ifdef OSK_NATIVE_HAS_SDL_HOST
/* Called only on the thread that owns SDL video and pumps its events (the
 * game window's owner). set_window(NULL) retires any dialog before the window
 * goes away. */
void osk_native_sdl_set_window(void* sdl_window);
void osk_native_sdl_pump(void);
/* Nonzero when the event belonged to the dialog (the caller drops it). */
int  osk_native_sdl_handle_event(const void* sdl_event);
/* Any thread: a dialog is queued or open and owns the keyboard. */
int  osk_native_sdl_input_active(void);
#ifdef PS3RECOMP_NATIVE_OSK_HEADLESS
int  osk_headless_available(void);
void osk_headless_open(const OskNativeRequest* request);
void osk_headless_close(uint64_t token);
void osk_headless_confirm(uint64_t token);
#endif
#endif

#ifdef __cplusplus
}
#endif
#endif

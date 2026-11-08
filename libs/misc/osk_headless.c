/*
 * ps3recomp - headless native OSK host (opt-in: PS3RECOMP_NATIVE_OSK_HEADLESS)
 *
 * For POSIX builds without a toolkit dialog. The shared lifecycle in
 * osk_native.c owns guest state and sysutil events. Here a dialog opens at
 * once with its request's initial text (after PS3RECOMP_OSK_INITIAL_TEXT is
 * applied) and finishes with that text only when the control mailbox confirms
 * the open generation (OSK_NATIVE_CONTROL_FILE, "confirm <token>"). Nothing
 * else can type into it; without a mailbox the dialog can only be aborted,
 * which is reported once per dialog on stderr.
 */
#include "osk_native.h"
#if defined(PS3RECOMP_NATIVE_OSK) && defined(PS3RECOMP_NATIVE_OSK_HEADLESS)
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static OskNativeRequest s_dialog;   /* token 0: none open */

int osk_native_available(void) { return 1; }

void osk_native_open(const OskNativeRequest* request)
{
    if (!osk_native_is_pending(request->token)) return;
    pthread_mutex_lock(&s_mu);
    s_dialog = *request;
    pthread_mutex_unlock(&s_mu);
    osk_native_did_open(request->token);
    const char* control = getenv("OSK_NATIVE_CONTROL_FILE");
    if (!control || !*control)
        fprintf(stderr, "[cellOskDialog] headless dialog token=%llu: no OSK_NATIVE_CONTROL_FILE, "
                        "so it cannot be confirmed (abort only)\n", (unsigned long long)request->token);
}

void osk_native_close(uint64_t token)
{
    pthread_mutex_lock(&s_mu);
    if (s_dialog.token == token) s_dialog.token = 0;
    pthread_mutex_unlock(&s_mu);
}

void osk_native_confirm(uint64_t token)
{
    pthread_mutex_lock(&s_mu);
    OskNativeRequest dialog = s_dialog;
    pthread_mutex_unlock(&s_mu);
    if (!token || dialog.token != token || !osk_native_is_open(token)) return;
    uint32_t n = 0;
    while (n < dialog.limit && n < 512 && dialog.initial[n]) n++;
    osk_native_did_finish(token, 0, dialog.initial, n);
}
#endif

/*
 * ps3recomp - macOS native OSK host (opt-in: PS3RECOMP_NATIVE_OSK_MACOS)
 *
 * Shows a cellOskDialog request as an AppKit sheet (an NSAlert with a text
 * field, or a secure field for password panels) on the window registered with
 * osk_native_macos_set_window -- the Metal backend registers its game window.
 * The shared lifecycle in osk_native.c owns all guest-visible state; this file
 * receives copied host-endian UTF-16 requests and reports OK/Cancel with
 * host-owned text. Every AppKit object is touched only on the main thread.
 *
 * Keyboard focus: the sheet's completion handler runs while the finishing
 * sheet is still attached, when makeKeyWindow has no effect yet. So the hand
 * back to the game window happens on the next main-queue turn, and only when
 * nothing else claimed focus meanwhile: the app is still active, the parent is
 * still the registered visible window with no other sheet, no newer dialog is
 * up, the current generation finished with a real OK/Cancel, and the user did
 * not pick another window or application while the sheet was open. Abort,
 * parent close and re-registration all cancel the hand-back; it never
 * activates the app.
 */
#import <AppKit/AppKit.h>
#include "osk_native.h"
#if defined(PS3RECOMP_NATIVE_OSK) && defined(PS3RECOMP_NATIVE_OSK_MACOS)
#include <stdatomic.h>

static _Atomic(int) s_available;      /* read off the main thread */
static _Atomic(int) s_input_active;   /* a sheet is up and owns the keyboard */
static __weak NSWindow* s_parent;
static NSAlert* s_alert;
static NSTextField* s_field;
static uint64_t s_ui_token;
static id s_close_observer;

void osk_native_macos_set_window(void* window)
{
    NSCAssert([NSThread isMainThread], @"OSK window registration must be on the main thread");
    if (s_close_observer) {
        [[NSNotificationCenter defaultCenter] removeObserver:s_close_observer];
        s_close_observer = nil;
    }
    s_parent = (__bridge NSWindow*)window;
    atomic_store(&s_available, s_parent != nil);
    if (s_parent)
        s_close_observer = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowWillCloseNotification object:s_parent queue:nil
                    usingBlock:^(NSNotification* n) {
            (void)n;
            atomic_store(&s_available, 0);
            s_parent = nil;
            if (s_alert) [NSApp endSheet:s_alert.window returnCode:NSModalResponseCancel];
        }];
}

int osk_native_available(void) { return atomic_load(&s_available); }
int osk_native_macos_input_active(void) { return atomic_load(&s_input_active); }

static NSString* string16(const uint16_t* p)
{
    NSUInteger n = 0;
    while (n < 512 && p[n]) n++;
    return [[NSString alloc] initWithCharacters:p length:n];
}

void osk_native_open(const OskNativeRequest* request)
{
    OskNativeRequest copy = *request;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!osk_native_is_pending(copy.token)) return;
        if (!s_parent || !atomic_load(&s_available)) {   /* parent closed before dispatch */
            osk_native_host_closed(copy.token);
            return;
        }
        s_ui_token = copy.token;
        atomic_store(&s_input_active, 1);
        s_alert = [NSAlert new];
        s_alert.messageText = @"Game text input";
        s_alert.informativeText = string16(copy.message);
        [s_alert addButtonWithTitle:@"OK"];
        [s_alert addButtonWithTitle:@"Cancel"];
        s_field = (copy.panels & 0x20000000u) ? [NSSecureTextField new] : [NSTextField new];
        s_field.frame = NSMakeRect(0, 0, 360, 24);
        s_field.stringValue = string16(copy.initial);
        s_alert.accessoryView = s_field;
        NSAlert* alert = s_alert;
        NSTextField* field = s_field;
        NSWindow* parent = s_parent;

        /* A user who picks another window or application while the sheet is
         * open keeps that choice, even if the window goes away before the
         * sheet completes. Notifications for a window that is no longer key
         * are ignored. */
        __block BOOL yielded_focus = NO;
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        id app_focus = [nc addObserverForName:NSApplicationDidResignActiveNotification object:NSApp
                                        queue:nil usingBlock:^(NSNotification* n) {
            (void)n;
            yielded_focus = YES;
        }];
        id window_focus = [nc addObserverForName:NSWindowDidBecomeKeyNotification object:nil
                                           queue:nil usingBlock:^(NSNotification* n) {
            if (n.object == NSApp.keyWindow && n.object != parent && n.object != alert.window)
                yielded_focus = YES;
        }];

        [alert beginSheetModalForWindow:parent completionHandler:^(NSModalResponse response) {
            NSString* text = field.stringValue;
            NSUInteger n = MIN(text.length, (NSUInteger)512);
            uint16_t units[513] = {0};
            [text getCharacters:units range:NSMakeRange(0, n)];
            osk_native_did_finish(copy.token, response == NSAlertFirstButtonReturn ? 0 : 1,
                                  units, (uint32_t)n);
            if (s_ui_token != copy.token) {
                [nc removeObserver:app_focus];
                [nc removeObserver:window_focus];
                return;
            }
            s_alert = nil;
            s_field = nil;
            s_ui_token = 0;
            atomic_store(&s_input_active, 0);
            dispatch_async(dispatch_get_main_queue(), ^{   /* after the sheet detaches */
                [nc removeObserver:app_focus];
                [nc removeObserver:window_focus];
                NSWindow* key = NSApp.keyWindow;
                if (!yielded_focus && NSApp.isActive && parent == s_parent && atomic_load(&s_available) &&
                    parent.isVisible && !parent.attachedSheet && s_ui_token == 0 &&
                    (!key || key == parent || key == alert.window) &&
                    osk_native_has_ui_result(copy.token)) {
                    [parent makeKeyWindow];
                    [parent makeFirstResponder:parent.contentView];
                }
            });
        }];
        [alert.window makeFirstResponder:field];
        osk_native_did_open(copy.token);
    });
}

void osk_native_close(uint64_t token)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        if (s_alert && s_ui_token == token)
            [NSApp endSheet:s_alert.window returnCode:NSModalResponseCancel];
    });
}

void osk_native_confirm(uint64_t token)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        /* An abort or a closing parent can race the queued command. */
        if (s_alert && s_ui_token == token && s_alert.window.sheetParent == s_parent &&
            s_parent && atomic_load(&s_available) && osk_native_is_open(token))
            [s_alert.buttons[0] performClick:nil];
    });
}
#endif

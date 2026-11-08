/* Real AppKit OSK host: a real sheet, text field and buttons on a real window,
 * with guest events delivered through the real cellOskDialog/cellSysutil poll.
 * Built and launched as an app bundle by tools/test_native_osk_macos.py, so
 * activation and keyboard ownership are real. No game files. */
#import <AppKit/AppKit.h>
#include "../libs/misc/cellOskDialog.h"
#include "../libs/system/cellSysutil.h"
#include "../libs/misc/osk_native.h"
#include "ps3emu/guest_call.h"
#include <assert.h>
#include <unistd.h>

uint8_t* vm_base; uint32_t ppu_vm_size = 65536, g_ww_lo, g_ww_hi; int g_resv_store_active;
void ppu_resv_break_store(uint64_t a) { (void)a; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int z) { (void)a; (void)v; (void)z; }
void vm_write32(uint64_t a, uint32_t v) { assert(a + 4 <= 65536); v = __builtin_bswap32(v); memcpy(vm_base + a, &v, 4); }
void vm_write8(uint64_t a, uint8_t v) { assert(a < 65536); vm_base[a] = v; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t a) { (void)a; }
void cellMsgDialog_tick(void) {}
void np_psnr_pump(void) {}
void ps3_np_eula_tick(void) {}

/* The game window's content view: counts the key events it receives. */
@interface GameView : NSView
@property unsigned keys;
@end
@implementation GameView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)e { (void)e; self.keys++; }
- (void)keyUp:(NSEvent*)e { (void)e; }
@end
@interface SentinelView : NSView
@end
@implementation SentinelView
- (BOOL)acceptsFirstResponder { return YES; }
@end

static NSWindow* s_game;
static GameView* s_view;
static unsigned events; static uint32_t codes[128];
static void cb(u32 opd, u64 code, u64 param, u64 user, u64 d, u64 e, u64 f, u64 g, u64 h)
{
    (void)opd; (void)param; (void)user; (void)d; (void)e; (void)f; (void)g; (void)h;
    assert(events < 128); codes[events++] = (uint32_t)code;
}

/* The runtime's pump: key events reach AppKit only while a sheet owns them. */
static void run_ui(void)
{
    for (int i = 0; i < 100; i++) {
        for (int e = 0; e < 16; e++) {
            NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
                                                inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!ev) break;
            if ((ev.type == NSEventTypeKeyDown || ev.type == NSEventTypeKeyUp) && !osk_native_macos_input_active())
                continue;
            [NSApp sendEvent:ev];
        }
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.002]];
    }
}
static NSWindow* alert_window(void) { return s_game.attachedSheet; }
static NSTextField* alert_field(void)
{
    NSWindow* w = alert_window();
    if (!w) return nil;
    NSMutableArray* stack = [NSMutableArray arrayWithObject:w.contentView];
    while (stack.count) {
        NSView* v = stack.lastObject; [stack removeLastObject];
        if ([v isKindOfClass:NSTextField.class] && ((NSTextField*)v).editable) return (NSTextField*)v;
        [stack addObjectsFromArray:v.subviews];
    }
    return nil;
}
static NSButton* alert_button(NSString* title)
{
    NSWindow* w = alert_window();
    NSMutableArray* stack = [NSMutableArray arrayWithObject:w.contentView];
    while (stack.count) {
        NSView* v = stack.lastObject; [stack removeLastObject];
        if ([v isKindOfClass:NSButton.class] && [((NSButton*)v).title isEqualToString:title]) return (NSButton*)v;
        [stack addObjectsFromArray:v.subviews];
    }
    return nil;
}
static void press(NSString* title) { NSButton* b = alert_button(title); assert(b); [b performClick:nil]; }
static void put16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(vm_base + a, &v, 2); }
static uint16_t get16(uint32_t a) { uint16_t v; memcpy(&v, vm_base + a, 2); return __builtin_bswap16(v); }
static uint32_t get32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static void result(void) { vm_write32(0x1200, 0xA5A5A5A5); vm_write32(0x1204, 32); vm_write32(0x1208, 0x2200); }
static int32_t load(void) { return cellOskDialogLoadAsync(-1, (void*)0x1100, (void*)0x1000); }
static void control_command(uint64_t token)
{
    const char* path = getenv("OSK_NATIVE_CONTROL_FILE"); assert(path);
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "w"); assert(f);
    assert(fprintf(f, "confirm %llu\n", (unsigned long long)token) > 0); assert(!fclose(f));
    assert(!rename(tmp, path));
}
static void wait_sheet_closed(void)
{
    for (int i = 0; i < 500 && alert_window(); i++)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.002]];
    assert(!alert_window());
}
static uint64_t current_token(void)
{
    for (uint64_t t = 1; t < 64; t++) if (osk_native_is_open(t)) return t;
    return 0;
}
/* The game window is key with its view as first responder, and keys reach it. */
static void game_keys(void)
{
    assert(NSApp.isActive && s_game.isKeyWindow && NSApp.keyWindow == s_game);
    assert(s_game.firstResponder == s_view);
    unsigned before = s_view.keys;
    NSEvent* down = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0
                                 windowNumber:s_game.windowNumber context:nil characters:@"x"
                  charactersIgnoringModifiers:@"x" isARepeat:NO keyCode:7];
    [NSApp sendEvent:down];
    assert(s_view.keys == before + 1);
}
static void wait_game_focus(void)
{
    for (int i = 0; i < 500 && (!s_game.isKeyWindow || s_game.firstResponder != s_view); i++)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.002]];
    game_keys();
}
static void focus_game(void)
{
    if (@available(macOS 14.0, *)) [NSApp activate]; else [NSApp activateIgnoringOtherApps:YES];
    [s_game makeKeyAndOrderFront:nil]; [s_game makeFirstResponder:s_view];
    for (int i = 0; i < 20 && (!NSApp.isActive || !s_game.isKeyWindow); i++) run_ui();
    game_keys();
}
static void guest_finish(uint32_t expected)
{
    cellSysutilCheckCallback(); result();
    assert(!cellOskDialogUnloadAsync((void*)0x1200)); assert(get32(0x1200) == expected);
    cellSysutilCheckCallback(); run_ui();
}

/* The hand-back of keyboard focus must respect every other owner. */
static void focus_races(void)
{
    vm_write32(0x1100, 0);
    NSWindow* other = [[NSWindow alloc] initWithContentRect:NSMakeRect(20, 20, 160, 90) styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered defer:NO];
    other.releasedWhenClosed = NO;
    NSTextField* other_field = [NSTextField new]; other.contentView = other_field;
    SentinelView* sentinel = [SentinelView new]; [s_view addSubview:sentinel];

    /* The user picks another window while the sheet is open: that choice stays. */
    focus_game(); assert(!load()); run_ui(); assert(alert_window());
    [other makeKeyAndOrderFront:nil]; [other makeFirstResponder:other_field]; run_ui();
    assert(other.isKeyWindow); NSResponder* selected = other.firstResponder;
    press(@"OK"); run_ui();
    assert(other.isKeyWindow && other.firstResponder == selected && !s_game.isKeyWindow); guest_finish(0);

    /* ... even when that window has gone away before the sheet completes. */
    focus_game(); assert([s_game makeFirstResponder:sentinel]);
    assert(!load()); run_ui();
    [other makeKeyAndOrderFront:nil]; run_ui(); assert(other.isKeyWindow); [other orderOut:nil]; run_ui();
    press(@"Cancel"); run_ui(); assert(s_game.firstResponder == sentinel); guest_finish(1);

    /* A follow-up sheet keeps the keyboard. */
    focus_game(); assert([s_game makeFirstResponder:sentinel]);
    assert(!load()); run_ui();
    NSWindow* follow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 160, 80) styleMask:NSWindowStyleMaskTitled
                                                     backing:NSBackingStoreBuffered defer:NO];
    follow.releasedWhenClosed = NO;
    NSButton* ok = alert_button(@"OK"); assert(ok);
    [s_game beginSheet:follow completionHandler:^(NSModalResponse r) { (void)r; }];   /* queued behind the OSK */
    [ok performClick:nil];
    for (int i = 0; i < 500 && s_game.attachedSheet != follow; i++)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.002]];
    run_ui();
    assert(s_game.attachedSheet == follow && follow.isKeyWindow && s_game.firstResponder == sentinel); guest_finish(0);
    [s_game endSheet:follow]; run_ui(); [follow close];

    /* Re-registering another window cancels the hand-back to the old one. */
    focus_game(); assert([s_game makeFirstResponder:sentinel]);
    assert(!load()); run_ui();
    osk_native_macos_set_window((__bridge void*)other);
    press(@"OK"); wait_sheet_closed(); run_ui(); assert(s_game.firstResponder == sentinel); guest_finish(0);
    osk_native_macos_set_window((__bridge void*)s_game);

    /* Another application takes activation while the sheet is open. */
    focus_game(); assert(!load()); run_ui();
    const char* other_app = getenv("OSK_FIXTURE_OTHER_APP"); assert(other_app);
    if (@available(macOS 14.0, *)) [NSApp yieldActivationToApplicationWithBundleIdentifier:@"org.ps3recomp.tests.nativeoskother"];
    __block NSRunningApplication* app = nil; __block NSError* error = nil; __block BOOL launched = NO;
    NSWorkspaceOpenConfiguration* config = [NSWorkspaceOpenConfiguration configuration];
    config.createsNewApplicationInstance = YES; config.arguments = @[@"--focus-other"];
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:other_app]]
                                          configuration:config
                                      completionHandler:^(NSRunningApplication* a, NSError* e) { app = a; error = e; launched = YES; }];
    for (int i = 0; i < 20 && !launched; i++) run_ui();
    assert(launched && app && !error);
    for (int i = 0; i < 20 && NSApp.isActive; i++) run_ui();
    assert(!NSApp.isActive && app.active);
    press(@"OK"); run_ui(); assert(!NSApp.isActive && !s_game.isKeyWindow); guest_finish(0);
    for (int i = 0; i < 20 && !app.terminated; i++) run_ui();
    assert(app.terminated);

    /* A sheet the guest aborted does not take focus back either. */
    focus_game(); assert([s_game makeFirstResponder:sentinel]);
    assert(!load()); run_ui(); assert(alert_window());
    assert(!cellOskDialogAbort()); run_ui(); assert(!alert_window() && s_game.firstResponder == sentinel); guest_finish(2);
    [sentinel removeFromSuperview]; [other close]; focus_game();
}

static void run_fixture(void)
{
    @autoreleasepool {
        const int controlled = getenv("OSK_NATIVE_CONTROL_FILE") && *getenv("OSK_NATIVE_CONTROL_FILE");
        vm_base = calloc(1, 65536); assert(vm_base);
        assert(osk_native_available()); game_keys();
        vm_write32(0x1000, 0x2000); vm_write32(0x1004, 0x2100); vm_write32(0x1008, 32);
        put16(0x2000, 'N'); put16(0x2002, 0);
        const uint16_t original[] = {'P', 'l', 'a', 'y', 'e', 'r', 0};
        for (unsigned i = 0; i < 7; i++) put16(0x2100 + i * 2, original[i]);
        assert(!setenv("PS3RECOMP_OSK_INITIAL_TEXT", "PlayerHost", 1));
        g_ps3_guest_caller = cb; cellSysutilRegisterCallback(1, (void*)0x3000, NULL);

        /* Unicode OK; nothing is reported to the guest before the user acts. */
        assert(!load()); assert(!alert_window());
        if (controlled) control_command(1);   /* a command before the sheet is up is discarded */
        run_ui();
        NSTextField* field = alert_field();
        assert(alert_window() && field && ![field isKindOfClass:NSSecureTextField.class]);
        assert([field.stringValue isEqualToString:@"PlayerHost"]);
        assert(osk_native_macos_input_active() && events == 0);
        /* Typed keys go through the pump to the sheet, not to the game view. */
        unsigned game_before = s_view.keys;
        [alert_window() makeFirstResponder:field];
        field.stringValue = @"";
        NSEvent* key = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0
                                    windowNumber:alert_window().windowNumber context:nil characters:@"q"
                     charactersIgnoringModifiers:@"q" isARepeat:NO keyCode:12];
        [NSApp postEvent:key atStart:NO]; run_ui();
        assert(s_view.keys == game_before);
        assert([field.stringValue isEqualToString:@"q"] || [field.currentEditor.string isEqualToString:@"q"]);
        field.stringValue = @"PΩ🚀";
        if (controlled) {
            assert(access(getenv("OSK_NATIVE_CONTROL_FILE"), F_OK));   /* the early command was consumed */
            uint64_t token = current_token(); assert(token);
            control_command(token + 1); cellOskDialog_tick(); run_ui(); assert(alert_window());
            control_command(token); cellOskDialog_tick();
            control_command(token); cellOskDialog_tick();   /* a repeat cannot press twice */
        } else {
            press(@"OK");
        }
        wait_sheet_closed(); assert(events == 0 && !osk_native_macos_input_active());
        wait_game_focus();
        cellSysutilCheckCallback(); assert(events == 2 && codes[0] == 0x502 && codes[1] == 0x503);
        result(); assert(!cellOskDialogUnloadAsync((void*)0x1200));
        assert(get32(0x1200) == 0 && get16(0x2200) == 'P' && get16(0x2202) == 0x03A9 &&
               get16(0x2204) == 0xD83D && get16(0x2206) == 0xDE80 && !get16(0x2208));
        cellSysutilCheckCallback(); assert(events == 3 && codes[2] == 0x504); run_ui();

        /* A password panel uses a secure field and is not prefilled by the override; Cancel. */
        vm_write32(0x1100, 0x20000000); assert(!load()); run_ui();
        assert([alert_field() isKindOfClass:NSSecureTextField.class]);
        assert([alert_field().stringValue isEqualToString:@"Player"]);
        press(@"Cancel"); wait_sheet_closed(); wait_game_focus(); guest_finish(1);

        if (controlled) {
            /* An abort after a queued confirm wins; a stale token cannot press the next sheet. */
            assert(!load()); run_ui(); assert(alert_window());
            uint64_t cancelled = current_token(); control_command(cancelled); cellOskDialog_tick();
            assert(!cellOskDialogAbort()); run_ui(); assert(!alert_window()); guest_finish(2);
            assert(!load()); run_ui(); assert(alert_window());
            control_command(cancelled); cellOskDialog_tick(); run_ui(); assert(alert_window());
            assert(!cellOskDialogAbort()); run_ui(); guest_finish(2);
        }

        focus_races();

        /* Abort before the sheet is shown. */
        assert(!load()); assert(!cellOskDialogAbort()); run_ui(); assert(!alert_window()); guest_finish(2);

        /* Closing the game window cancels its sheet and makes the host unavailable. */
        assert(!load()); run_ui(); assert(alert_window());
        [s_game close]; assert(!osk_native_available()); run_ui(); assert(!alert_window());
        guest_finish(1); assert(!osk_native_macos_input_active());
        free(vm_base);
        if (controlled) puts("PASS control mailbox: early, wrong-token and repeated commands; abort race; stale token");
        puts("PASS real AppKit OSK: Unicode OK, secure Cancel, typed keys routed to the sheet, focus hand-back guards, abort, parent close");
        const char* receipt = getenv("OSK_FIXTURE_RECEIPT");
        if (receipt) { FILE* done = fopen(receipt, "w"); assert(done); assert(fputs("passed\n", done) >= 0); assert(!fclose(done)); }
    }
}

int main(int argc, char** argv)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if (argc == 2 && !strcmp(argv[1], "--focus-other")) {   /* the second application */
            NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(30, 30, 160, 80) styleMask:NSWindowStyleMaskTitled
                                                        backing:NSBackingStoreBuffered defer:NO];
            w.releasedWhenClosed = NO; [w makeKeyAndOrderFront:nil];
            [NSTimer scheduledTimerWithTimeInterval:.05 repeats:NO block:^(NSTimer* t) {
                (void)t; if (@available(macOS 14.0, *)) [NSApp activate]; else [NSApp activateIgnoringOtherApps:YES];
            }];
            [NSTimer scheduledTimerWithTimeInterval:1.5 repeats:NO block:^(NSTimer* t) {
                (void)t;
                if (@available(macOS 14.0, *)) [NSApp yieldActivationToApplicationWithBundleIdentifier:@"org.ps3recomp.tests.nativeosk"];
                [NSApp terminate:nil];
            }];
            [NSApp run];
            return 0;
        }
        /* Take real startup activation in AppKit's own loop, then leave it: the
         * runtime pumps events itself, and so does the rest of this fixture. */
        [NSTimer scheduledTimerWithTimeInterval:.05 repeats:NO block:^(NSTimer* t) {
            (void)t;
            s_game = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 270)
                                                 styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                   backing:NSBackingStoreBuffered defer:NO];
            s_game.releasedWhenClosed = NO;
            s_view = [GameView new]; s_game.contentView = s_view;
            [s_game center]; [s_game makeKeyAndOrderFront:nil]; [s_game makeFirstResponder:s_view];
            osk_native_macos_set_window((__bridge void*)s_game);
            if (@available(macOS 14.0, *)) [NSApp activate]; else [NSApp activateIgnoringOtherApps:YES];
            __block unsigned ticks = 0;
            [NSTimer scheduledTimerWithTimeInterval:.05 repeats:YES block:^(NSTimer* startup) {
                if (NSApp.isActive && s_game.isKeyWindow && NSApp.keyWindow == s_game) {
                    [startup invalidate]; [NSApp stop:nil]; run_fixture(); [NSApp terminate:nil];
                } else if (++ticks == 100) {
                    fprintf(stderr, "fixture did not get startup focus: active=%d key=%d\n", NSApp.isActive, s_game.isKeyWindow);
                    assert(!"no startup focus");
                }
            }];
        }];
        [NSApp run];
    }
    return 0;
}

/* SDL2 native OSK dialog: a real SDL window and renderer (dummy, offscreen or
 * a private X server) driven by injected SDL events. The guest side runs the
 * OSK lifecycle (LoadAsync without Init, sysutil 0x502/0x503/0x504) on its
 * own thread; this main thread owns SDL video and pumps it, as the game
 * window's thread does. Checks UTF-16 results, the length limit, cancel, the
 * masked password view, IME composition, the mailbox confirm, abort, window
 * teardown, and (in the headless build) the no-window fallback. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include "../libs/misc/cellOskDialog.h"
#include "../libs/system/cellSysutil.h"
#include "ps3emu/guest_call.h"
#include "../libs/misc/osk_native.h"
#ifndef OSK_NATIVE_HAS_SDL_HOST
#error "build with -DPS3RECOMP_NATIVE_OSK_SDL=1"
#endif
int osk_native_sdl_describe(char* field, size_t cap, uint32_t* window_id);

uint8_t memory[65536]; uint8_t* vm_base = memory; uint32_t ppu_vm_size = sizeof(memory);
uint32_t g_ww_lo, g_ww_hi; int g_resv_store_active;
void ppu_resv_break_store(uint64_t a) { (void)a; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int z) { (void)a; (void)v; (void)z; }
void vm_write32(uint64_t a, uint32_t v) { assert(a + 4 <= sizeof(memory)); v = __builtin_bswap32(v); memcpy(memory + a, &v, 4); }
void vm_write8(uint64_t a, uint8_t v) { assert(a < sizeof(memory)); memory[a] = v; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t a) { (void)a; }
void cellMsgDialog_tick(void) {}
void np_psnr_pump(void) {}
void ps3_np_eula_tick(void) {}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static _Atomic(uint32_t) statuses[64];
static _Atomic(unsigned) events;
static void cb(u32 opd, u64 status, u64 param, u64 user, u64 d, u64 e, u64 f, u64 g, u64 h)
{
    (void)opd; (void)param; (void)user; (void)d; (void)e; (void)f; (void)g; (void)h;
    unsigned i = atomic_fetch_add(&events, 1);
    CHECK(i < 64);
    atomic_store(&statuses[i], (uint32_t)status);
}
static void put16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(memory + a, &v, 2); }
static uint32_t read32(uint32_t a) { uint32_t x; memcpy(&x, memory + a, 4); return __builtin_bswap32(x); }
static uint16_t read16(uint32_t a) { uint16_t x; memcpy(&x, memory + a, 2); return __builtin_bswap16(x); }
static void put_ascii(uint32_t a, const char* s) { while (*s) { put16(a, (uint8_t)*s++); a += 2; } put16(a, 0); }

/* ---- the guest: one dialog per job, run on its own thread ---------------- */
typedef struct {
    const char* message; const char* initial; uint32_t limit, panels, prohibit;
    int32_t load_result;      /* expected LoadAsync result */
    int abort_after_open;     /* the guest aborts once 0x502 arrives */
    /* outputs */
    uint32_t result; uint16_t text[600]; unsigned length; int done;
} Job;
static Job* volatile s_job;
static _Atomic(int) s_job_done;

static void wait_status(unsigned index, uint32_t status)
{
    for (int i = 0; i < 5000 && atomic_load(&events) <= index; i++) { cellSysutilCheckCallback(); usleep(1000); }
    CHECK(atomic_load(&events) > index);
    CHECK(atomic_load(&statuses[index]) == status);
}

static void guest_run(Job* job)
{
    memset(memory, 0, sizeof memory);
    vm_write32(0x1000, 0x2000); vm_write32(0x1004, 0x2400); vm_write32(0x1008, job->limit);
    put_ascii(0x2000, job->message); put_ascii(0x2400, job->initial);
    vm_write32(0x1100, job->panels); vm_write32(0x1110, job->prohibit);
    unsigned base = atomic_load(&events);
    int32_t r = cellOskDialogLoadAsync(-1, (void*)0x1100, (void*)0x1000);   /* no Init */
    CHECK(r == job->load_result);
    if (r) { job->done = 1; return; }
    wait_status(base, 0x502);
    if (job->abort_after_open) CHECK(cellOskDialogAbort() == 0);
    wait_status(base + 1, 0x503);
    vm_write32(0x1200, 0xA5A5A5A5); vm_write32(0x1204, 600); vm_write32(0x1208, 0x3000);
    CHECK(cellOskDialogUnloadAsync((void*)0x1200) == 0);
    job->result = read32(0x1200);
    for (job->length = 0; job->length < 599 && read16(0x3000 + 2 * job->length); job->length++)
        job->text[job->length] = read16(0x3000 + 2 * job->length);
    wait_status(base + 2, 0x504);
    job->done = 1;
}

static void* guest_thread(void* arg)
{
    (void)arg;
    for (;;) {
        Job* job = s_job;
        if (job == (Job*)1) return NULL;
        if (job && !atomic_load(&s_job_done)) { guest_run(job); atomic_store(&s_job_done, 1); }
        usleep(500);
    }
}

/* ---- the owner: this thread pumps SDL, like the game window's pump ------- */
static SDL_Window* s_parent;
static int s_parent_saw_keys;

static void pump(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (osk_native_sdl_handle_event(&e)) continue;
        if (e.type == SDL_KEYDOWN || e.type == SDL_TEXTINPUT) s_parent_saw_keys++;
    }
    osk_native_sdl_pump();
}

static void start(Job* job)
{
    atomic_store(&s_job_done, 0);
    s_job = job;
}

static void finish_job(void)
{
    for (int i = 0; i < 10000 && !atomic_load(&s_job_done); i++) { pump(); usleep(500); }
    CHECK(atomic_load(&s_job_done));
    s_job = NULL;
    pump();
    CHECK(!osk_native_sdl_describe(NULL, 0, NULL));
}

/* Wait for the dialog to appear; returns its window id. */
static uint32_t await_dialog(char* field, size_t cap)
{
    uint32_t id = 0;
    for (int i = 0; i < 10000 && !osk_native_sdl_describe(field, cap, &id); i++) { pump(); usleep(500); }
    CHECK(id != 0);
    return id;
}

/* sdl2-compat (SDL2 API over SDL3, versions 2.32.50 and later) crashes when
 * an application pushes a text event, so there the event is handed to the
 * owner's dispatch exactly as SDL_PollEvent would return it. Real SDL2 queues
 * it like any other event. */
static int s_text_through_queue;
static void deliver_text_event(SDL_Event* e)
{
    if (s_text_through_queue) { CHECK(SDL_PushEvent(e) == 1); pump(); return; }
    if (!osk_native_sdl_handle_event(e)) s_parent_saw_keys++;
    pump();
}

static void text(uint32_t window, const char* utf8)
{
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = SDL_TEXTINPUT; e.text.windowID = window;
    SDL_strlcpy(e.text.text, utf8, sizeof e.text.text);
    deliver_text_event(&e);
}

static void editing(uint32_t window, const char* utf8)
{
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = SDL_TEXTEDITING; e.edit.windowID = window;
    SDL_strlcpy(e.edit.text, utf8, sizeof e.edit.text);
    e.edit.length = (Sint32)strlen(utf8);
    deliver_text_event(&e);
}

static void key(uint32_t window, SDL_Keycode sym, Uint16 mod)
{
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN; e.key.windowID = window; e.key.state = SDL_PRESSED;
    e.key.keysym.sym = sym; e.key.keysym.scancode = SDL_GetScancodeFromKey(sym); e.key.keysym.mod = mod;
    CHECK(SDL_PushEvent(&e) == 1);
    e.type = SDL_KEYUP; e.key.state = SDL_RELEASED;
    CHECK(SDL_PushEvent(&e) == 1);
    pump();
}

static void field_is(const char* want)
{
    char field[600]; uint32_t id;
    CHECK(osk_native_sdl_describe(field, sizeof field, &id));
    if (strcmp(field, want)) { fprintf(stderr, "field '%s' != '%s'\n", field, want); exit(1); }
}

static void text_is(const Job* job, const uint16_t* want, unsigned n)
{
    CHECK(job->length == n);
    for (unsigned i = 0; i < n; i++) CHECK(job->text[i] == want[i]);
}

/* Publish one mailbox command the way the automation does: complete in a
 * temporary file, then renamed into place, so the poll never sees a partial
 * command (the guest thread polls every millisecond). */
static void publish(const char* control, const char* line)
{
    char temporary[4200];
    CHECK(snprintf(temporary, sizeof temporary, "%s.tmp", control) < (int)sizeof temporary);
    FILE* f = fopen(temporary, "w"); CHECK(f); CHECK(fputs(line, f) >= 0); CHECK(!fclose(f));
    CHECK(rename(temporary, control) == 0);
}

int main(void)
{
    const char* control = getenv("OSK_NATIVE_CONTROL_FILE");
    CHECK(control && *control);
    g_ps3_guest_caller = cb;
    CHECK(!cellSysutilRegisterCallback(1, (void*)0x3000, (void*)0xDEAD));
    pthread_t guest;
    CHECK(pthread_create(&guest, NULL, guest_thread, NULL) == 0);

#ifdef PS3RECOMP_NATIVE_OSK_HEADLESS
    /* No window yet: the headless host takes the request; only the mailbox
     * confirms it, with the initial text. */
    {
        CHECK(osk_native_available());
        Job job = {"Name", "Fallback", 16, 0, 0, 0, 0};
        start(&job);
        for (int i = 0; i < 2000 && atomic_load(&events) == 0; i++) usleep(1000);
        CHECK(atomic_load(&events) == 1 && atomic_load(&statuses[0]) == 0x502);
        publish(control, "confirm 1\n");
        finish_job();
        static const uint16_t want[] = {'F','a','l','l','b','a','c','k'};
        CHECK(job.result == 0); text_is(&job, want, 8);
        puts("PASS headless fallback without an SDL window (mailbox confirm, initial text)");
    }
#else
    /* No window: unavailable, as on the other hosts without their window. */
    {
        CHECK(!osk_native_available());
        Job job = {"Name", "", 16, 0, 0, (int32_t)0x8002B503, 0};
        start(&job); finish_job();
        puts("PASS no SDL window: OSK reports unavailable");
    }
#endif

    CHECK(SDL_InitSubSystem(SDL_INIT_VIDEO) == 0);
    SDL_version linked; SDL_GetVersion(&linked);
    s_text_through_queue = !(linked.major == 2 && linked.minor == 32 && linked.patch >= 50);
    printf("SDL video driver: %s (SDL %u.%u.%u, text events %s)\n", SDL_GetCurrentVideoDriver(),
           linked.major, linked.minor, linked.patch,
           s_text_through_queue ? "through SDL_PushEvent" : "dispatched directly (sdl2-compat)");
    s_parent = SDL_CreateWindow("parent", 0, 0, 320, 200, SDL_WINDOW_SHOWN);
    CHECK(s_parent);
    osk_native_sdl_set_window(s_parent);
    CHECK(osk_native_available());

    /* 1. Typing, limit in UTF-16 units, non-ASCII shown as '?', Backspace over
     *    a surrogate pair, Enter accepts. Also: nothing appears until the
     *    owner pumps, and another thread's pump is ignored. */
    {
        Job job = {"Enter your name", "ab", 5, 0, 0, 0, 0};
        start(&job);
        for (int i = 0; i < 2000 && !osk_native_sdl_input_active(); i++) usleep(1000);
        CHECK(osk_native_sdl_input_active());
        CHECK(!osk_native_sdl_describe(NULL, 0, NULL));
        pthread_t other;
        CHECK(pthread_create(&other, NULL, (void* (*)(void*))osk_native_sdl_pump, NULL) == 0);
        pthread_join(other, NULL);
        CHECK(!osk_native_sdl_describe(NULL, 0, NULL));
        char field[64];
        uint32_t id = await_dialog(field, sizeof field);
        CHECK(!strcmp(field, "ab"));
        text(id, "cd\xC3\xA9");          field_is("abcd?");      /* e-acute: 5 units */
        text(id, "Z");                   field_is("abcd?");      /* over the limit */
        key(id, SDLK_BACKSPACE, 0);      field_is("abcd");
        text(id, "\xF0\x9F\x9A\x80");    field_is("abcd");       /* pair needs 2 units */
        key(id, SDLK_BACKSPACE, 0);      text(id, "\xF0\x9F\x9A\x80"); field_is("abc?");
        key(id, SDLK_BACKSPACE, 0);      field_is("abc");        /* whole pair removed */
        text(id, "\xF0\x9F\x9A\x80");    key(id, SDLK_RETURN, 0);
        finish_job();
        static const uint16_t want[] = {'a','b','c',0xD83D,0xDE80};
        CHECK(job.result == 0); text_is(&job, want, 5);
        CHECK(s_parent_saw_keys == 0);
        puts("PASS typed UTF-16 text, limit, backspace, Enter accepts");
    }

    /* 2. Escape cancels even when the keyboard event names the game window,
     *    and the game window never sees it. */
    {
        Job job = {"Name", "keep", 32, 0, 0, 0, 0};
        start(&job);
        char field[64];
        await_dialog(field, sizeof field);
        uint32_t parent = SDL_GetWindowID(s_parent);
        text(parent, "x");               field_is("keepx");
        key(parent, SDLK_ESCAPE, 0);
        finish_job();
        CHECK(job.result == 1 && job.length == 0);
        CHECK(s_parent_saw_keys == 0);
        puts("PASS Escape cancels (keyboard routed from the game window)");
    }

    /* 3. Closing the dialog window cancels. */
    {
        Job job = {"Name", "", 32, 0, 0, 0, 0};
        start(&job);
        char field[64];
        uint32_t id = await_dialog(field, sizeof field);
        SDL_Event e; memset(&e, 0, sizeof e);
        e.type = SDL_WINDOWEVENT; e.window.windowID = id; e.window.event = SDL_WINDOWEVENT_CLOSE;
        CHECK(SDL_PushEvent(&e) == 1);
        finish_job();
        CHECK(job.result == 1 && job.length == 0);
        puts("PASS window close cancels");
    }

    /* 4. Password panel: masked view, real text returned; Ctrl+V pastes. */
    {
        Job job = {"Password", "", 32, 0x20000000u, 0, 0, 0};
        start(&job);
        char field[64];
        uint32_t id = await_dialog(field, sizeof field);
        text(id, "s3cr");
        SDL_SetClipboardText("et");
        char* clip = SDL_GetClipboardText();
        int clip_ok = clip && !strcmp(clip, "et");
        SDL_free(clip);
        key(id, SDLK_v, KMOD_LCTRL);
        if (clip_ok) field_is("******"); else { field_is("****"); text(id, "et"); }
        key(id, SDLK_KP_ENTER, 0);
        finish_job();
        static const uint16_t want[] = {'s','3','c','r','e','t'};
        CHECK(job.result == 0); text_is(&job, want, 6);
        printf("PASS password masked, real text returned (clipboard paste %s)\n",
               clip_ok ? "checked" : "not supported by this video driver");
    }

    /* 5. IME composition shows but is not committed; Enter while composing
     *    belongs to the IME; the committed text is returned. */
    {
        Job job = {"Name", "", 32, 0, 0, 0, 0};
        start(&job);
        char field[64];
        uint32_t id = await_dialog(field, sizeof field);
        editing(id, "ka");
        key(id, SDLK_RETURN, 0);
        CHECK(osk_native_sdl_describe(NULL, 0, NULL));   /* still open */
        field_is("");
        text(id, "\xE3\x81\x8B");        field_is("?");
        key(id, SDLK_RETURN, 0);
        finish_job();
        static const uint16_t want[] = {0x304B};
        CHECK(job.result == 0); text_is(&job, want, 1);
        puts("PASS IME composition then commit");
    }

    /* 6. Prohibited spaces never enter the field. Mouse on OK accepts. */
    {
        Job job = {"Name", "", 32, 0, 1, 0, 0};
        start(&job);
        char field[64];
        uint32_t id = await_dialog(field, sizeof field);
        text(id, "a b");                 field_is("ab");
        SDL_Event e; memset(&e, 0, sizeof e);
        e.type = SDL_MOUSEBUTTONDOWN; e.button.windowID = id; e.button.button = SDL_BUTTON_LEFT;
        e.button.state = SDL_PRESSED; e.button.clicks = 1; e.button.x = 360; e.button.y = 190;
        CHECK(SDL_PushEvent(&e) == 1);
        finish_job();
        static const uint16_t want[] = {'a','b'};
        CHECK(job.result == 0); text_is(&job, want, 2);
        puts("PASS prohibited space filtered, OK button accepts");
    }

    /* 7. The automation mailbox presses the same OK path. A wrong generation
     *    is ignored. */
    {
        Job job = {"Name", "auto", 32, 0, 0, 0, 0};
        start(&job);
        char field[64];
        uint32_t id = await_dialog(field, sizeof field);
        text(id, "mated");
        /* Tokens count accepted LoadAsync calls: six above, plus the headless
         * one (the unavailable attempt takes none). */
#ifdef PS3RECOMP_NATIVE_OSK_HEADLESS
        const unsigned token = 8;
#else
        const unsigned token = 7;
#endif
        char line[32];
        snprintf(line, sizeof line, "confirm %u\n", token + 1);
        publish(control, line);
        for (int i = 0; i < 200; i++) { pump(); usleep(1000); }
        CHECK(osk_native_sdl_describe(NULL, 0, NULL));   /* still open */
        snprintf(line, sizeof line, "confirm %u\n", token);
        publish(control, line);
        finish_job();
        static const uint16_t want[] = {'a','u','t','o','m','a','t','e','d'};
        CHECK(job.result == 0); text_is(&job, want, 9);
        CHECK(access(control, F_OK) != 0);
        puts("PASS mailbox confirm through the dialog's OK path");
    }

    /* 8. The guest aborts an open dialog: result 2 and the window goes. */
    {
        Job job = {"Name", "zz", 32, 0, 0, 0, 1};
        start(&job);
        finish_job();
        CHECK(job.result == 2 && job.length == 0);
        puts("PASS guest abort closes the dialog");
    }

    /* 9. The game window goes away under an open dialog: aborted, and the
     *    OSK reports unavailable afterwards (headless fallback if built). */
    {
        Job job = {"Name", "", 32, 0, 0, 0, 0};
        start(&job);
        char field[64];
        await_dialog(field, sizeof field);
        osk_native_sdl_set_window(NULL);
        finish_job();
        CHECK(job.result == 2);
#ifndef PS3RECOMP_NATIVE_OSK_HEADLESS
        CHECK(!osk_native_available());
#endif
        puts("PASS window teardown aborts the open dialog");
    }

    s_job = (Job*)1;
    pthread_join(guest, NULL);
    SDL_DestroyWindow(s_parent);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    puts("native SDL OSK: all cases passed");
    return 0;
}

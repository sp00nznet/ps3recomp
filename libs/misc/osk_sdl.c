/* SDL2 text-entry dialog for the native OSK lifecycle on hosts without a
 * toolkit dialog (Linux and other non-Apple, non-Windows POSIX systems).
 *
 * Behaviour:
 *  - The guest thread only queues work here (open/close/confirm) and reads one
 *    atomic. It never touches SDL.
 *  - The thread that registered the game window (osk_native_sdl_set_window)
 *    owns SDL video and its event pump. It alone creates, draws, feeds and
 *    destroys the dialog, from osk_native_sdl_pump/handle_event.
 *  - Results reach the guest only through the shared lifecycle in
 *    osk_native.c (did_open / did_finish / host_closed); the guest's own
 *    CheckCallback poll turns those into sysutil events.
 *
 * The dialog is a small separate window: a title, the guest's prompt, one
 * edit field holding the guest's initial text, and OK/Cancel. Enter or OK
 * accepts; Escape, Cancel or closing the window cancels. Text arrives through
 * SDL text input (SDL_TEXTINPUT, with SDL_TEXTEDITING composition shown but
 * not committed), Backspace deletes one character and Ctrl+V pastes. The
 * guest's length limit is enforced in UTF-16 code units at entry, and a
 * password request (secure panel bit) is shown masked. SDL has no fonts, so
 * an embedded public-domain 8x8 bitmap font draws printable ASCII; anything
 * else is drawn as '?' while the real text is kept and returned as UTF-16.
 *
 * While a dialog is open it owns the keyboard: key and text events from any
 * of this process's windows are routed to it, so a compositor that leaves
 * focus on the game window still lets the user type, and Escape never reaches
 * the game window's quit handling.
 *
 * With no registered window (no display, or the game window disabled) the
 * request goes to the headless mailbox host when it is built
 * (PS3RECOMP_NATIVE_OSK_HEADLESS); otherwise the OSK reports unavailable. */
#include "osk_native.h"
#ifdef OSK_NATIVE_HAS_SDL_HOST
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "osk_font8x8.h"

#define OSK_SECURE_PANEL 0x20000000u /* password field, as the other hosts read it */
#define OSK_CAP 512

/* ---- guest -> owner queue (s_mu) ------------------------------------- */
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static OskNativeRequest s_request;
static int s_open_pending;
static uint64_t s_close_pending, s_confirm_pending;
static _Atomic(int) s_available, s_input_active;
#ifdef PS3RECOMP_NATIVE_OSK_HEADLESS
static _Atomic(uint64_t) s_headless_token; /* generation handed to the headless host */
#endif

/* ---- owner-thread state ------------------------------------------------ */
static pthread_t s_owner;
static _Atomic(int) s_owner_set;
static SDL_Window* s_parent;
static SDL_Window* s_win;
static SDL_Renderer* s_ren;
static SDL_Texture* s_font;
static Uint32 s_win_id;
static OskNativeRequest s_cur;
static uint64_t s_generation;
static uint16_t s_text[OSK_CAP + 1];
static uint32_t s_len;
static char s_compose[64];
static int s_dirty, s_text_input_was_active;

/* Layout, in window coordinates (the renderer's logical size). */
enum { W = 600, H = 236, MARGIN = 20, SCALE = 2, GLYPH = 8 * SCALE,
       MSG_Y = 16, MSG_LINES = 3, MSG_STEP = 20,
       FIELD_Y = 86, FIELD_H = 38, HINT_Y = 134,
       BTN_Y = 176, BTN_H = 40, OK_X = 352, OK_W = 104, CANCEL_X = 466, CANCEL_W = 114 };
#define FIELD_COLS ((W - 2 * MARGIN - 16) / GLYPH - 1) /* leave room for the caret */
#define MSG_COLS ((W - 2 * MARGIN) / GLYPH)

static int on_owner(void)
{
    return atomic_load(&s_owner_set) && pthread_equal(pthread_self(), s_owner);
}

static void refresh_input_active(void)
{
    pthread_mutex_lock(&s_mu);
    atomic_store(&s_input_active, s_win != NULL || s_open_pending);
    pthread_mutex_unlock(&s_mu);
}

int osk_native_sdl_input_active(void) { return atomic_load(&s_input_active); }

/* ---- text model --------------------------------------------------------- */
static int allowed(uint32_t cp)
{
    if (cp < 0x20 || cp == 0x7F) return 0;               /* controls never enter the field */
    if ((s_cur.prohibit & 1) && cp == ' ') return 0;     /* the lifecycle drops these too */
    return !(cp >= 0xD800 && cp <= 0xDFFF) && cp <= 0x10FFFF;
}

static void insert_codepoint(uint32_t cp)
{
    if (!allowed(cp)) return;
    uint32_t units = cp > 0xFFFF ? 2 : 1;
    if (s_len + units > s_cur.limit || s_len + units > OSK_CAP) return;
    if (units == 2) {
        cp -= 0x10000;
        s_text[s_len++] = (uint16_t)(0xD800 + (cp >> 10));
        s_text[s_len++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
    } else {
        s_text[s_len++] = (uint16_t)cp;
    }
    s_text[s_len] = 0;
    s_dirty = 1;
}

/* Insert UTF-8; malformed bytes are skipped rather than guessed at. */
static void insert_utf8(const char* text)
{
    const unsigned char* p = (const unsigned char*)text;
    while (*p) {
        uint32_t cp = *p++, minimum = 0;
        unsigned more = 0;
        if (cp < 0x80) {}
        else if (cp >= 0xC2 && cp <= 0xDF) { cp &= 0x1F; more = 1; minimum = 0x80; }
        else if (cp >= 0xE0 && cp <= 0xEF) { cp &= 0x0F; more = 2; minimum = 0x800; }
        else if (cp >= 0xF0 && cp <= 0xF4) { cp &= 0x07; more = 3; minimum = 0x10000; }
        else continue;
        unsigned i = 0;
        for (; i < more && (*p & 0xC0) == 0x80; i++) cp = (cp << 6) | (*p++ & 0x3F);
        if (i < more || cp < minimum) continue;
        insert_codepoint(cp);
    }
}

static void delete_last(void)
{
    if (!s_len) return;
    s_len--;
    if (s_len && s_text[s_len] >= 0xDC00 && s_text[s_len] <= 0xDFFF &&
        s_text[s_len - 1] >= 0xD800 && s_text[s_len - 1] <= 0xDBFF)
        s_len--;
    s_text[s_len] = 0;
    s_dirty = 1;
}

/* One display cell per code point: printable ASCII as itself, else '?'. */
static size_t display_cells(const uint16_t* text, size_t n, int masked, char* out, size_t cap,
                            int keep_newlines)
{
    size_t k = 0;
    for (size_t i = 0; i < n && text[i] && k + 1 < cap; i++) {
        uint16_t u = text[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) i++;
        char c = (u >= 0x20 && u < 0x7F) ? (char)u : '?';
        if (u == '\n' && keep_newlines) c = '\n';
        else if (u == '\t' || u == '\r' || u == '\n') c = ' ';
        out[k++] = masked ? '*' : c;
    }
    out[k] = 0;
    return k;
}

/* ---- drawing ------------------------------------------------------------ */
static void fill(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_Rect rc = {x, y, w, h};
    SDL_SetRenderDrawColor(s_ren, r, g, b, 255);
    SDL_RenderFillRect(s_ren, &rc);
}

static void frame(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_Rect rc = {x, y, w, h};
    SDL_SetRenderDrawColor(s_ren, r, g, b, 255);
    SDL_RenderDrawRect(s_ren, &rc);
}

static void draw_text(int x, int y, int scale, const char* s, size_t n, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetTextureColorMod(s_font, r, g, b);
    for (size_t i = 0; i < n && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E) c = '?';
        SDL_Rect src = {(int)(c - 0x20) * 8, 0, 8, 8};
        SDL_Rect dst = {x + (int)i * 8 * scale, y, 8 * scale, 8 * scale};
        SDL_RenderCopy(s_ren, s_font, &src, &dst);
    }
}

static void draw_centered(int x, int y, int w, int h, const char* s, Uint8 v)
{
    int n = (int)strlen(s);
    draw_text(x + (w - n * GLYPH) / 2, y + (h - GLYPH) / 2, SCALE, s, (size_t)n, v, v, v);
}

/* Greedy word wrap of the prompt into at most MSG_LINES lines. */
static void draw_message(void)
{
    char cells[OSK_CAP + 1];
    size_t n = display_cells(s_cur.message, OSK_CAP, 0, cells, sizeof cells, 1), i = 0;
    for (int line = 0; line < MSG_LINES && i < n; line++) {
        while (i < n && cells[i] == ' ') i++;
        size_t start = i, end = i;
        while (end < n && cells[end] != '\n' && end - start < (size_t)MSG_COLS) end++;
        size_t next = end;
        if (end < n && cells[end] != '\n' && cells[end] != ' ') {
            size_t space = end;
            while (space > start && cells[space - 1] != ' ') space--;
            if (space > start) { end = space - 1; next = space; }
        }
        if (next < n && cells[next] == '\n') next++;
        char buf[MSG_COLS + 1];
        size_t len = end - start;
        memcpy(buf, cells + start, len);
        if (line == MSG_LINES - 1 && next < n && len >= 3) memcpy(buf + len - 3, "...", 3);
        draw_text(MARGIN, MSG_Y + line * MSG_STEP, SCALE, buf, len, 235, 235, 235);
        i = next;
    }
}

static void render(void)
{
    char cells[OSK_CAP + 72];
    const int masked = (s_cur.panels & OSK_SECURE_PANEL) != 0;
    size_t n = display_cells(s_text, s_len, masked, cells, sizeof cells, 0);
    /* The uncommitted composition follows the text, never masked: it is the
     * IME's own preview and is not part of the result until committed. */
    size_t committed = n;
    for (const char* c = s_compose; *c && n + 1 < sizeof cells; c++)
        if (((unsigned char)*c & 0xC0) != 0x80) cells[n++] = ((unsigned char)*c < 0x80 && *c >= 0x20) ? *c : '?';
    cells[n] = 0;
    size_t first = n > (size_t)FIELD_COLS ? n - FIELD_COLS : 0;

    fill(0, 0, W, H, 40, 42, 52);
    draw_message();
    fill(MARGIN, FIELD_Y, W - 2 * MARGIN, FIELD_H, 18, 18, 24);
    frame(MARGIN, FIELD_Y, W - 2 * MARGIN, FIELD_H, 120, 150, 220);
    const int tx = MARGIN + 8, ty = FIELD_Y + (FIELD_H - GLYPH) / 2;
    if (committed > first)
        draw_text(tx, ty, SCALE, cells + first, committed - first, 255, 255, 255);
    size_t cfrom = committed > first ? committed : first;
    if (n > cfrom) {
        int cx = tx + (int)(cfrom - first) * GLYPH;
        draw_text(cx, ty, SCALE, cells + cfrom, n - cfrom, 250, 210, 120);
        fill(cx, ty + GLYPH + 1, (int)(n - cfrom) * GLYPH, 2, 250, 210, 120);
    }
    fill(tx + (int)(n - first) * GLYPH + 1, ty - 2, 2, GLYPH + 4, 120, 150, 220);
    char hint[64];
    int hn = snprintf(hint, sizeof hint, "Enter: OK   Esc: Cancel   %u/%u", s_len, s_cur.limit);
    draw_text(MARGIN, HINT_Y, 1, hint, hn > 0 ? (size_t)hn : 0, 160, 160, 170);
    fill(OK_X, BTN_Y, OK_W, BTN_H, 60, 90, 160);
    frame(OK_X, BTN_Y, OK_W, BTN_H, 140, 170, 230);
    draw_centered(OK_X, BTN_Y, OK_W, BTN_H, "OK", 255);
    fill(CANCEL_X, BTN_Y, CANCEL_W, BTN_H, 70, 70, 80);
    frame(CANCEL_X, BTN_Y, CANCEL_W, BTN_H, 150, 150, 160);
    draw_centered(CANCEL_X, BTN_Y, CANCEL_W, BTN_H, "Cancel", 235);
    SDL_RenderPresent(s_ren);
    s_dirty = 0;
}

/* ---- dialog lifetime (owner thread) ------------------------------------ */
static void destroy_dialog(void)
{
    uint64_t generation = s_generation;
    s_generation = 0;
    if (s_win) {
        if (!s_text_input_was_active) SDL_StopTextInput();
        if (s_font) SDL_DestroyTexture(s_font);
        if (s_ren) SDL_DestroyRenderer(s_ren);
        SDL_DestroyWindow(s_win);
        s_win = NULL; s_ren = NULL; s_font = NULL; s_win_id = 0;
        if (s_parent) SDL_RaiseWindow(s_parent);  /* hand the keyboard back */
    }
    /* A password must not outlive its dialog. */
    SDL_memset(s_text, 0, sizeof s_text);
    SDL_memset(s_compose, 0, sizeof s_compose);
    SDL_memset(&s_cur, 0, sizeof s_cur);
    s_len = 0;
    if (generation) osk_native_host_closed(generation); /* no-op after a result */
    refresh_input_active();
}

static void finish(int result)
{
    if (!s_win || !osk_native_is_open(s_generation)) return;
    osk_native_did_finish(s_generation, result, s_text, result == 0 ? s_len : 0);
    destroy_dialog();
}

static SDL_Texture* make_font(void)
{
    static Uint32 pixels[8][95 * 8];
    for (int g = 0; g < 95; g++)
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                pixels[row][g * 8 + col] = (osk_font8x8[g][row] >> col) & 1 ? 0xFFFFFFFFu : 0u;
    SDL_Texture* t = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 95 * 8, 8);
    if (!t) return NULL;
    if (SDL_UpdateTexture(t, NULL, pixels, (int)sizeof pixels[0]) != 0) { SDL_DestroyTexture(t); return NULL; }
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static int create_dialog(const OskNativeRequest* request)
{
    int display = SDL_GetWindowDisplayIndex(s_parent);
    if (display < 0) display = 0;
    s_win = SDL_CreateWindow("Game text input", SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                             SDL_WINDOWPOS_CENTERED_DISPLAY(display), W, H,
                             SDL_WINDOW_SHOWN | SDL_WINDOW_ALWAYS_ON_TOP);
    if (!s_win) {
        fprintf(stderr, "[cellOskDialog] SDL dialog window failed: %s\n", SDL_GetError());
        return 0;
    }
    s_win_id = SDL_GetWindowID(s_win);
    SDL_SetWindowModalFor(s_win, s_parent);   /* transient for the game window where supported */
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
    if (!s_ren) s_ren = SDL_CreateRenderer(s_win, -1, 0);
    if (s_ren) {
        SDL_RenderSetLogicalSize(s_ren, W, H);
        s_font = make_font();
    }
    if (!s_ren || !s_font) {
        fprintf(stderr, "[cellOskDialog] SDL dialog renderer failed: %s\n", SDL_GetError());
        destroy_dialog();
        return 0;
    }
    s_cur = *request;
    s_generation = request->token;
    s_len = 0;
    while (s_len < s_cur.limit && s_len < OSK_CAP && request->initial[s_len]) {
        s_text[s_len] = request->initial[s_len];
        s_len++;
    }
    s_text[s_len] = 0;
    s_compose[0] = 0;
    s_text_input_was_active = SDL_IsTextInputActive() == SDL_TRUE;
    SDL_StartTextInput();
    SDL_Rect field = {MARGIN, FIELD_Y, W - 2 * MARGIN, FIELD_H};
    SDL_SetTextInputRect(&field);
    SDL_RaiseWindow(s_win);
    s_dirty = 1;
    render();
    printf("[cellOskDialog] SDL dialog open token=%llu limit=%u%s\n",
           (unsigned long long)request->token, request->limit,
           (request->panels & OSK_SECURE_PANEL) ? " (masked)" : "");
    fflush(stdout);
    osk_native_did_open(request->token);
    return 1;
}

/* ---- owner-thread entry points ------------------------------------------ */
void osk_native_sdl_set_window(void* value)
{
    SDL_Window* parent = (SDL_Window*)value;
    atomic_store(&s_available, 0);
    atomic_store(&s_owner_set, 0);
    pthread_mutex_lock(&s_mu);
    uint64_t pending = s_open_pending ? s_request.token : 0;
    if (pending > s_close_pending) s_close_pending = pending;
    if (s_generation > s_close_pending) s_close_pending = s_generation;
    s_open_pending = 0; s_confirm_pending = 0;
    pthread_mutex_unlock(&s_mu);
    if (pending) osk_native_host_closed(pending);
    /* The caller owns the window lifecycle, so a dialog is retired on the
     * caller's thread just before the window (and SDL video) goes away. */
    destroy_dialog();
    s_parent = parent;
    if (parent) {
        s_owner = pthread_self();
        atomic_store(&s_owner_set, 1);
    }
    refresh_input_active();
    atomic_store(&s_available, parent != NULL);
    if (parent)
        printf("[cellOskDialog] SDL text-entry dialog host ready (video driver %s)\n",
               SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
}

void osk_native_sdl_pump(void)
{
    if (!on_owner()) return;
    pthread_mutex_lock(&s_mu);
    int opening = s_open_pending;
    OskNativeRequest request = s_request;
    s_open_pending = 0;
    uint64_t closing = s_close_pending, confirming = s_confirm_pending;
    s_confirm_pending = 0;
    pthread_mutex_unlock(&s_mu);
    /* A later closed generation also retires every older queued UI. */
    if (closing && opening && closing >= request.token) opening = 0;
    if (closing && s_generation && closing >= s_generation) destroy_dialog();
    if (opening && osk_native_is_pending(request.token)) {
        if (s_win) destroy_dialog();     /* single-window lifecycle: an older UI is stale */
        if (!s_parent || !atomic_load(&s_available) || !create_dialog(&request))
            osk_native_host_closed(request.token);
    }
    /* The automation mailbox presses the same OK path a user does. */
    if (confirming && s_win && confirming == s_generation && osk_native_is_open(confirming))
        finish(0);
    refresh_input_active();
    if (s_win && s_dirty) render();
}

static int keyboard_event(Uint32 type)
{
    return type == SDL_KEYDOWN || type == SDL_KEYUP || type == SDL_TEXTINPUT || type == SDL_TEXTEDITING;
}

static Uint32 event_window(const SDL_Event* e)
{
    switch (e->type) {
    case SDL_WINDOWEVENT: return e->window.windowID;
    case SDL_KEYDOWN: case SDL_KEYUP: return e->key.windowID;
    case SDL_TEXTINPUT: return e->text.windowID;
    case SDL_TEXTEDITING: return e->edit.windowID;
    case SDL_MOUSEMOTION: return e->motion.windowID;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: return e->button.windowID;
    case SDL_MOUSEWHEEL: return e->wheel.windowID;
    default: return 0;
    }
}

static int inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

int osk_native_sdl_handle_event(const void* value)
{
    const SDL_Event* e = (const SDL_Event*)value;
    if (!e || !on_owner() || !s_win) return 0;
    const Uint32 window = event_window(e);
    if (window != s_win_id && !keyboard_event(e->type)) return 0;
    switch (e->type) {
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_CLOSE) finish(1);
        else s_dirty = 1;
        break;
    case SDL_TEXTINPUT:
        insert_utf8(e->text.text);
        s_compose[0] = 0;
        break;
    case SDL_TEXTEDITING:
        SDL_strlcpy(s_compose, e->edit.text, sizeof s_compose);
        s_dirty = 1;
        break;
    case SDL_KEYDOWN: {
        const SDL_Keycode key = e->key.keysym.sym;
        const int composing = s_compose[0] != 0;
        if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && !e->key.repeat && !composing) finish(0);
        else if (key == SDLK_ESCAPE && !e->key.repeat && !composing) finish(1);
        else if (key == SDLK_BACKSPACE && !composing) delete_last();
        else if ((key == SDLK_v && (e->key.keysym.mod & KMOD_CTRL)) ||
                 (key == SDLK_INSERT && (e->key.keysym.mod & KMOD_SHIFT))) {
            char* clip = SDL_GetClipboardText();
            if (clip) { insert_utf8(clip); SDL_free(clip); }
        }
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
            if (inside(e->button.x, e->button.y, OK_X, BTN_Y, OK_W, BTN_H)) finish(0);
            else if (inside(e->button.x, e->button.y, CANCEL_X, BTN_Y, CANCEL_W, BTN_H)) finish(1);
        }
        break;
    default:
        break;
    }
    if (s_win && s_dirty) render();
    return 1;
}

/* Synthetic-test view of the open dialog: what the field shows (masked or
 * '?'-substituted) and its window id. Returns 0 when no dialog is open. */
int osk_native_sdl_describe(char* field, size_t cap, uint32_t* window_id)
{
    if (!on_owner() || !s_win) return 0;
    if (field && cap)
        display_cells(s_text, s_len, (s_cur.panels & OSK_SECURE_PANEL) != 0, field, cap, 0);
    if (window_id) *window_id = s_win_id;
    return 1;
}

/* ---- host entry points (any thread) ------------------------------------- */
static int headless_available(void)
{
#ifdef PS3RECOMP_NATIVE_OSK_HEADLESS
    return osk_headless_available();
#else
    return 0;
#endif
}

int osk_native_available(void)
{
    if (atomic_load(&s_available) || headless_available()) return 1;
    static _Atomic(int) said;
    if (!atomic_exchange(&said, 1))
        fprintf(stderr, "[cellOskDialog] native OSK unavailable: no SDL game window "
                        "(no display, or PS3RECOMP_VK_WINDOW is not set)\n");
    return 0;
}

void osk_native_open(const OskNativeRequest* request)
{
    if (!osk_native_is_pending(request->token)) return;
#ifdef PS3RECOMP_NATIVE_OSK_HEADLESS
    if (!atomic_load(&s_available) && headless_available()) {
        atomic_store(&s_headless_token, request->token);
        osk_headless_open(request);
        return;
    }
#endif
    pthread_mutex_lock(&s_mu);
    if (!atomic_load(&s_available)) {
        pthread_mutex_unlock(&s_mu);
        osk_native_host_closed(request->token);
        return;
    }
    if (request->token <= s_close_pending || request->token < s_request.token) {
        pthread_mutex_unlock(&s_mu);
        return;
    }
    s_request = *request;
    s_open_pending = 1;
    atomic_store(&s_input_active, 1);
    pthread_mutex_unlock(&s_mu);
}

void osk_native_close(uint64_t token)
{
#ifdef PS3RECOMP_NATIVE_OSK_HEADLESS
    if (token && atomic_load(&s_headless_token) == token) osk_headless_close(token);
#endif
    pthread_mutex_lock(&s_mu);
    if (token > s_close_pending) s_close_pending = token;
    pthread_mutex_unlock(&s_mu);
}

void osk_native_confirm(uint64_t token)
{
    if (!osk_native_is_open(token)) return;
#ifdef PS3RECOMP_NATIVE_OSK_HEADLESS
    if (atomic_load(&s_headless_token) == token) { osk_headless_confirm(token); return; }
#endif
    pthread_mutex_lock(&s_mu);
    if (token > s_confirm_pending) s_confirm_pending = token;
    pthread_mutex_unlock(&s_mu);
}
#endif

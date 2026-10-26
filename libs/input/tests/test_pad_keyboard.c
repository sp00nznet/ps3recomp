/* cellPad's keyboard fallback on port 0 (the SDL source): with no controller
 * plugged in the keyboard plays the pad -- arrows = d-pad, Z/X/A/S = cross/
 * circle/square/triangle, Q/W = L1/R1, 1/2 = L2/R2, Return = START, Tab =
 * SELECT, the left stick mirrors the d-pad -- while a window of the process has
 * the keyboard focus, and releases everything when it loses it.
 *
 *   clang -std=gnu17 -DPS3RECOMP_PAD_KEYBOARD_SDL -I include $(sdl2-config --cflags) \
 *         -I "$(sdl2-config --prefix)/include" libs/input/tests/test_pad_keyboard.c \
 *         $(sdl2-config --libs) -o /tmp/test_pad_keyboard && /tmp/test_pad_keyboard
 *
 * PS3RECOMP_PAD_KEYBOARD_SDL makes macOS read the keyboard through SDL like
 * every other host that is not Windows, so the same table is tested everywhere.
 * No controller exists and no window either: the test defines the SDL calls the
 * backend makes (they take precedence over libSDL2's), with a keyboard state
 * and a focus flag it sets itself.
 */
#include "../cellPad.c"
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
u32 cellGcm_flip_request_count(void) { return 0; }   /* PAD_CLOCK=flips */
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

/* ---- the fake SDL: no controller, a keyboard the test drives ----------------- */
static Uint8 s_keys[SDL_NUM_SCANCODES];
static int   s_focus;

int SDL_NumJoysticks(void) { return 0; }
int SDL_PeepEvents(SDL_Event* ev, int n, SDL_eventaction action, Uint32 lo, Uint32 hi)
{
    (void)ev; (void)n; (void)action; (void)lo; (void)hi;
    return 0;
}
Uint32 SDL_WasInit(Uint32 flags) { (void)flags; return flags; }
int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
const char* SDL_GetError(void) { return "fake"; }
void SDL_GameControllerUpdate(void) {}
const Uint8* SDL_GetKeyboardState(int* numkeys) { if (numkeys) *numkeys = SDL_NUM_SCANCODES; return s_keys; }
SDL_Window* SDL_GetKeyboardFocus(void) { return s_focus ? (SDL_Window*)&s_keys : NULL; }

/* ---- helpers ---------------------------------------------------------------- */
/* One 16-bit word of cellPadGetData(0) (0 if it reported nothing new). */
static unsigned data_word(int offset)
{
    const uint32_t ea = 0x2000;
    usleep((PAD_POLL_INTERVAL_MS + 2) * 1000);
    assert(cellPadGetData(0, (CellPadData*)(uintptr_t)ea) == CELL_OK);
    assert(vm_base[ea + 3] != 0);   /* len > 0: a report was handed out */
    const uint8_t* b = vm_base + ea + 4 + offset * 2;
    return (unsigned)b[0] << 8 | b[1];
}
#define D1() data_word(CELL_PAD_BTN_OFFSET_DIGITAL1)
#define D2() data_word(CELL_PAD_BTN_OFFSET_DIGITAL2)
static void keys(int a, int b, int c)
{
    memset(s_keys, 0, sizeof s_keys);
    if (a) s_keys[a] = 1;
    if (b) s_keys[b] = 1;
    if (c) s_keys[c] = 1;
}
/* DIGITAL1 / DIGITAL2 bits */
#define SELECT 0x01u
#define START  0x08u
#define UP     0x10u
#define RIGHT  0x20u
#define DOWN   0x40u
#define LEFT   0x80u
#define L2     0x01u
#define R2     0x02u
#define L1     0x04u
#define R1     0x08u
#define TRI    0x10u
#define CIRCLE 0x20u
#define CROSS  0x40u
#define SQUARE 0x80u

int main(void)
{
    vm_base = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);
    assert(cellPadInit(4) == CELL_OK);
    s_focus = 1;

    /* Nothing pressed: neutral, sticks centred (0 would be full deflection). */
    keys(0, 0, 0);
    assert(D1() == 0 && D2() == 0);
    assert(data_word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) == 128);
    assert(data_word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y) == 128);

    /* The face and shoulder buttons. */
    static const struct { int key; unsigned bit; } d2[] = {
        { SDL_SCANCODE_Z, CROSS }, { SDL_SCANCODE_X, CIRCLE }, { SDL_SCANCODE_A, SQUARE },
        { SDL_SCANCODE_S, TRI },   { SDL_SCANCODE_Q, L1 },     { SDL_SCANCODE_W, R1 },
        { SDL_SCANCODE_1, L2 },    { SDL_SCANCODE_2, R2 },
    };
    for (unsigned i = 0; i < sizeof d2 / sizeof d2[0]; i++) {
        keys(d2[i].key, 0, 0);
        const unsigned v = D2();
        assert(v == d2[i].bit);
        keys(0, 0, 0);
        assert(D2() == 0);
    }
    keys(SDL_SCANCODE_RETURN, 0, 0);
    assert(D1() == START);
    keys(SDL_SCANCODE_TAB, 0, 0);
    assert(D1() == SELECT);

    /* The d-pad, and the left stick that mirrors it. */
    keys(SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, 0);
    assert(D1() == (LEFT | UP));
    assert(data_word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) == 0);
    assert(data_word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y) == 0);
    keys(SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN, 0);
    assert(D1() == (RIGHT | DOWN));
    assert(data_word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) == 255);
    assert(data_word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y) == 255);

    /* Several keys at once. */
    keys(SDL_SCANCODE_Z, SDL_SCANCODE_RIGHT, SDL_SCANCODE_RETURN);
    assert((D2() & CROSS) && (D1() == (RIGHT | START)));

    /* The window loses the keyboard focus with keys still down: everything is
     * released, or a key held while switching to another program would stay
     * pressed for good. */
    keys(SDL_SCANCODE_Z, SDL_SCANCODE_LEFT, 0);
    s_focus = 0;
    assert(D1() == 0 && D2() == 0);
    assert(data_word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) == 128);
    s_focus = 1;
    assert(D2() == CROSS);

    assert(cellPadEnd() == CELL_OK);
    printf("test_pad_keyboard: ok\n");
    return 0;
}

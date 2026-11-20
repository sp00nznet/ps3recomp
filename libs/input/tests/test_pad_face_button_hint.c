/* cellPad's SDL backend asks SDL for face buttons by position, and does it
 * before the game-controller subsystem starts.
 *
 * SDL 2 maps a Nintendo controller by its printed labels unless
 * SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS is "0", which swaps Cross and
 * Circle (and Square and Triangle) against a PlayStation pad. The test records
 * every SDL_SetHint and SDL_InitSubSystem the backend makes, in order.
 *
 *   clang -std=gnu17 -I include $(sdl2-config --cflags) -I "$(sdl2-config --prefix)/include" \
 *         libs/input/tests/test_pad_face_button_hint.c $(sdl2-config --libs) -o /tmp/test_pad_face_button_hint && /tmp/test_pad_face_button_hint
 *
 * No controller is opened: the test defines the SDL calls the backend makes
 * (they take precedence over libSDL2's) over a system with no controllers.
 */
#include "../cellPad.c"
#include <assert.h>
#include <sys/mman.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
u32 cellGcm_flip_request_count(void) { return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

/* ---- a fake SDL that keeps the order things were done in ------------------- */
static int s_inited;                       /* the controller subsystem is up */
static int s_init_calls;
static int s_labels_hint_seen;             /* a USE_BUTTON_LABELS hint was set ... */
static int s_labels_hint_before_init;      /* ... before the subsystem started */
static char s_labels_hint[16];

SDL_bool SDL_SetHint(const char* name, const char* value)
{
    if (strcmp(name, SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS) == 0) {
        s_labels_hint_seen++;
        s_labels_hint_before_init = !s_inited;
        snprintf(s_labels_hint, sizeof s_labels_hint, "%s", value);
    }
    return SDL_TRUE;
}
Uint32 SDL_WasInit(Uint32 flags) { return s_inited ? flags : 0; }
int SDL_InitSubSystem(Uint32 flags)
{
    assert(flags == SDL_INIT_GAMECONTROLLER);
    s_init_calls++;
    s_inited = 1;
    return 0;
}
int SDL_NumJoysticks(void) { return 0; }
SDL_bool SDL_IsGameController(int idx) { (void)idx; return SDL_FALSE; }
int SDL_PeepEvents(SDL_Event* ev, int n, SDL_eventaction action, Uint32 lo, Uint32 hi)
{ (void)ev; (void)n; (void)action; (void)lo; (void)hi; return 0; }
void SDL_GameControllerUpdate(void) {}
const char* SDL_GetError(void) { return "fake"; }

int main(void)
{
    vm_base = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);

    /* Something else already started the controller subsystem: the hint is
     * still set, and the backend does not start it again. */
    s_inited = 1;
    assert(cellPadInit(4) == CELL_OK);
    assert(s_init_calls == 0);
    assert(s_labels_hint_seen == 1);
    assert(strcmp(s_labels_hint, "0") == 0);
    assert(cellPadEnd() == CELL_OK);

    /* Nobody did: the hint is "0" and was set before the subsystem started. */
    s_inited = 0;
    s_sdl_inited = 0;
    s_labels_hint_seen = 0;
    assert(cellPadInit(4) == CELL_OK);
    assert(s_init_calls == 1);
    assert(s_labels_hint_seen == 1);
    assert(strcmp(s_labels_hint, "0") == 0);
    assert(s_labels_hint_before_init);
    assert(cellPadEnd() == CELL_OK);

    printf("test_pad_face_button_hint: ok\n");
    return 0;
}

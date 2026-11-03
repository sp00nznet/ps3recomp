/* cellPadSetActDirect -> host rumble: the motor levels, the per-port state,
 * and the renewal from the pad poll that keeps a long rumble going.
 *
 *   clang -std=gnu17 -I include $(sdl2-config --cflags) -I "$(sdl2-config --prefix)/include" \
 *         libs/input/tests/test_pad_rumble.c $(sdl2-config --libs) -o /tmp/test_pad_rumble && /tmp/test_pad_rumble
 *
 * No controller is opened. The test defines the SDL game-controller calls the
 * SDL backend makes (they take precedence over libSDL2's), hands port 0 a fake
 * controller, and records what reaches SDL_GameControllerRumble.
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
u32 cellGcm_flip_request_count(void) { return 0; }   /* PAD_CLOCK=flips (#344) */
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_linelock_lock(uint32_t a) { (void)a; }
void spu_linelock_release(uint32_t a) { (void)a; }

/* The fake controller. */
static int s_fake_gc, s_gone_gc;
static int s_calls;
static Uint16 s_lo, s_hi;
static Uint32 s_ms;
int SDL_GameControllerRumble(SDL_GameController* gc, Uint16 lo, Uint16 hi, Uint32 ms)
{
    assert(gc == (SDL_GameController*)&s_fake_gc);
    s_calls++; s_lo = lo; s_hi = hi; s_ms = ms;
    return 0;
}
void SDL_GameControllerUpdate(void) {}
SDL_bool SDL_IsGameController(int i) { (void)i; return SDL_FALSE; }
SDL_bool SDL_GameControllerGetAttached(SDL_GameController* gc) { return gc == (SDL_GameController*)&s_fake_gc; }
Uint8 SDL_GameControllerGetButton(SDL_GameController* gc, SDL_GameControllerButton b) { (void)gc; (void)b; return 0; }
Sint16 SDL_GameControllerGetAxis(SDL_GameController* gc, SDL_GameControllerAxis a) { (void)gc; (void)a; return 0; }
void SDL_GameControllerClose(SDL_GameController* gc) { (void)gc; }

static void test_levels(void)
{
    u16 lo, hi;
    pad_rumble_levels(0, 0, &lo, &hi);     assert(lo == 0 && hi == 0);
    pad_rumble_levels(1, 255, &lo, &hi);   assert(lo == 65535 && hi == 65535);
    pad_rumble_levels(1, 0, &lo, &hi);     assert(lo == 0 && hi == 65535);   /* small motor: on is full, not 1/255 */
    pad_rumble_levels(255, 0, &lo, &hi);   assert(hi == 65535);              /* any non-zero is on */
    pad_rumble_levels(0, 128, &lo, &hi);   assert(lo == 128 * 257 && hi == 0);
}

static void set_act(u32 port, u8 small, u8 large, s32 want)
{
    CellPadActParam* p = (CellPadActParam*)(vm_base + 0x100);
    p->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] = small;
    p->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] = large;
    assert(cellPadSetActDirect(port, (CellPadActParam*)(uintptr_t)0x100) == want);
}

static void test_set_act_direct(void)
{
    vm_base = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);
    s_pad_initialized = 1;

    /* No controller on port 0: the state is kept, nothing is sent. */
    set_act(0, 1, 200, CELL_OK);
    assert(s_rumble[0].lo == 200 * 257 && s_rumble[0].hi == 65535);
    assert(s_calls == 0 && s_rumble[0].sent_ms == 0);

    /* A controller: the request goes out at full small-motor strength and
     * with a duration long enough to be renewed before it runs out. */
    s_sdl_controllers[0] = (SDL_GameController*)&s_fake_gc;
    set_act(0, 1, 0, CELL_OK);
    assert(s_calls == 1 && s_lo == 0 && s_hi == 65535);
    assert(s_ms == PAD_RUMBLE_HOLD_MS && PAD_RUMBLE_RENEW_MS < PAD_RUMBLE_HOLD_MS);
    assert(s_rumble[0].sent_ms != 0);

    /* The poll renews it once PAD_RUMBLE_RENEW_MS has passed, not before. */
    pad_poll_sdl2();
    assert(s_calls == 1);
    s_rumble[0].sent_ms -= PAD_RUMBLE_RENEW_MS;
    pad_poll_sdl2();
    assert(s_calls == 2 && s_lo == 0 && s_hi == 65535 && s_ms == PAD_RUMBLE_HOLD_MS);
    assert(pad_now_ms() - s_rumble[0].sent_ms < PAD_RUMBLE_RENEW_MS);

    /* Zero stops the motors and ends the renewal. */
    set_act(0, 0, 0, CELL_OK);
    assert(s_calls == 3 && s_lo == 0 && s_hi == 0 && s_ms == 0);
    assert(s_rumble[0].sent_ms == 0);
    pad_poll_sdl2();
    assert(s_calls == 3);

    /* A controller that goes away drops its port's rumble. */
    set_act(0, 0, 255, CELL_OK);
    assert(s_calls == 4 && s_lo == 65535 && s_hi == 0);
    s_sdl_controllers[0] = (SDL_GameController*)&s_gone_gc;   /* GetAttached says no */
    pad_poll_sdl2();
    assert(s_sdl_controllers[0] == NULL && s_rumble[0].lo == 0 && s_rumble[0].sent_ms == 0);

    /* Ports past the host's, bad arguments, a closed library. */
    set_act(5, 1, 1, CELL_OK);
    set_act(CELL_PAD_MAX_PORT_NUM, 1, 1, (s32)CELL_PAD_ERROR_INVALID_PARAMETER);
    assert(cellPadSetActDirect(0, NULL) == (s32)CELL_PAD_ERROR_INVALID_PARAMETER);
    s_pad_initialized = 0;
    set_act(0, 1, 1, (s32)CELL_PAD_ERROR_NOT_OPENED);
}

int main(void)
{
    test_levels();
    test_set_act_direct();
    printf("test_pad_rumble: ok\n");
    return 0;
}

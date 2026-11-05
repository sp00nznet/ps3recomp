/* cellPad's SDL controller hot-plug: a controller switched off and on again
 * (a DualSense over Bluetooth comes back with a new instance id) is opened
 * again without a restart, into the port it had; a new one goes into a free
 * port, also when nobody delivered its device event; the ports report the
 * change once (CELL_PAD_STATUS_ASSIGN_CHANGES), and no rumble is left over.
 *
 *   clang -std=gnu17 -I include $(sdl2-config --cflags) -I "$(sdl2-config --prefix)/include" \
 *         libs/input/tests/test_pad_hotplug.c $(sdl2-config --libs) -o /tmp/test_pad_hotplug && /tmp/test_pad_hotplug
 *
 * No controller is opened: the test defines every SDL call the backend makes
 * (they take precedence over libSDL2's), over a fake device list that behaves
 * like Homebrew's SDL2 (sdl2-compat on SDL3): a device index resolves through
 * a list refreshed only by SDL_NumJoysticks, so code that never calls it
 * keeps seeing the switched-off controller.
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

/* ---- the fake SDL ----------------------------------------------------------- */
typedef struct {
    int      id;           /* SDL instance id; a reconnect gets a new one */
    int      guid;         /* which physical pad */
    int      present;      /* in the system's device list */
    uint32_t buttons;      /* SDL_GameControllerButton bits */
    int      opens;        /* times SDL_GameControllerOpen returned it */
    int      closed;       /* its handle was closed */
    unsigned rumble_lo, rumble_hi, rumble_calls;
} FakePad;
static FakePad s_fp[8];
static int s_nfp;
static int s_cached[8], s_ncached;   /* sdl2-compat's index list */
static Uint32 s_events[64];
static int s_nevents;

static FakePad* fake_new(int id, int guid)
{
    FakePad* p = &s_fp[s_nfp++];
    memset(p, 0, sizeof *p);
    p->id = id; p->guid = guid; p->present = 1;
    return p;
}
static void fake_event(Uint32 type) { s_events[s_nevents++] = type; }
static FakePad* fake_by_id(int id)
{
    for (int i = 0; i < s_nfp; i++) if (s_fp[i].id == id) return &s_fp[i];
    return NULL;
}

int SDL_NumJoysticks(void)
{
    s_ncached = 0;
    for (int i = 0; i < s_nfp; i++) if (s_fp[i].present) s_cached[s_ncached++] = s_fp[i].id;
    return s_ncached;
}
static FakePad* fake_at(int idx) { return idx >= 0 && idx < s_ncached ? fake_by_id(s_cached[idx]) : NULL; }
SDL_bool SDL_IsGameController(int idx) { FakePad* p = fake_at(idx); return p && p->present ? SDL_TRUE : SDL_FALSE; }
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int idx) { FakePad* p = fake_at(idx); return p ? p->id : -1; }
SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int idx)
{
    SDL_JoystickGUID g; memset(&g, 0, sizeof g);
    FakePad* p = fake_at(idx);
    if (p) g.data[0] = (Uint8)p->guid;
    return g;
}
const char* SDL_JoystickNameForIndex(int idx) { (void)idx; return "Fake DualSense"; }
SDL_GameController* SDL_GameControllerOpen(int idx)
{
    FakePad* p = fake_at(idx);
    if (!p || !p->present) return NULL;
    p->opens++; p->closed = 0;
    return (SDL_GameController*)p;
}
void SDL_GameControllerClose(SDL_GameController* gc) { ((FakePad*)gc)->closed = 1; }
SDL_bool SDL_GameControllerGetAttached(SDL_GameController* gc) { return ((FakePad*)gc)->present ? SDL_TRUE : SDL_FALSE; }
const char* SDL_GameControllerName(SDL_GameController* gc) { (void)gc; return "Fake DualSense"; }
SDL_bool SDL_GameControllerHasRumble(SDL_GameController* gc) { (void)gc; return SDL_TRUE; }
int SDL_GameControllerRumble(SDL_GameController* gc, Uint16 lo, Uint16 hi, Uint32 ms)
{
    (void)ms;
    FakePad* p = (FakePad*)gc;
    assert(!p->closed);
    p->rumble_lo = lo; p->rumble_hi = hi; p->rumble_calls++;
    return 0;
}
Uint8 SDL_GameControllerGetButton(SDL_GameController* gc, SDL_GameControllerButton b)
{
    return (Uint8)((((FakePad*)gc)->buttons >> b) & 1u);
}
Sint16 SDL_GameControllerGetAxis(SDL_GameController* gc, SDL_GameControllerAxis a) { (void)gc; (void)a; return 0; }
void SDL_GameControllerUpdate(void) {}
int SDL_PeepEvents(SDL_Event* ev, int n, SDL_eventaction action, Uint32 lo, Uint32 hi)
{
    assert(action == SDL_GETEVENT);
    int got = 0, keep = 0;
    for (int i = 0; i < s_nevents; i++) {
        if (got < n && s_events[i] >= lo && s_events[i] <= hi) {
            memset(&ev[got], 0, sizeof ev[got]);
            ev[got++].type = s_events[i];
        } else {
            s_events[keep++] = s_events[i];
        }
    }
    s_nevents = keep;
    return got;
}
Uint32 SDL_WasInit(Uint32 flags) { (void)flags; return flags; }
int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
const char* SDL_GetError(void) { return "fake"; }

/* ---- helpers ---------------------------------------------------------------- */
static void poll_once(void)
{
    usleep((PAD_POLL_INTERVAL_MS + 2) * 1000);
    cellPad_poll();
}
/* port_status[port] from cellPadGetInfo2 (guest big-endian). */
static u32 info_status(u32 port)
{
    const uint32_t ea = 0x1000;
    assert(cellPadGetInfo2((CellPadInfo2*)(uintptr_t)ea) == CELL_OK);
    const uint8_t* b = vm_base + ea + 12 + port * 4;
    return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}
/* DIGITAL2 of cellPadGetData(port) (0 if it reported nothing new). */
static unsigned data_digital2(u32 port)
{
    const uint32_t ea = 0x2000;
    usleep((PAD_POLL_INTERVAL_MS + 2) * 1000);
    assert(cellPadGetData(port, (CellPadData*)(uintptr_t)ea) == CELL_OK);
    const uint8_t* b = vm_base + ea + 4 + CELL_PAD_BTN_OFFSET_DIGITAL2 * 2;
    return (unsigned)b[0] << 8 | b[1];
}
#define CROSS_BIT  0x40u   /* in DIGITAL2 */
#define CIRCLE_BIT 0x20u

/* ---- the tests -------------------------------------------------------------- */
int main(void)
{
    vm_base = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);

    /* A DualSense is on at start: port 0. */
    FakePad* a = fake_new(1, 0xA5);
    assert(cellPadInit(4) == CELL_OK);
    assert(a->opens == 1 && s_sdl_controllers[0] == (SDL_GameController*)a);
    assert(info_status(0) == CELL_PAD_STATUS_CONNECTED);   /* what is there at start: no change flag */
    a->buttons = 1u << SDL_CONTROLLER_BUTTON_A;
    assert(data_digital2(0) & CROSS_BIT);
    a->buttons = 0;

    /* The title asks for rumble: it reaches the controller. */
    CellPadActParam* act = (CellPadActParam*)(vm_base + 0x3000);
    act->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] = 1;
    act->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] = 255;
    assert(cellPadSetActDirect(0, (CellPadActParam*)(uintptr_t)0x3000) == CELL_OK);
    assert(a->rumble_lo == 65535 && a->rumble_hi == 65535);

    /* Switched off: SDL drops it (and says so with an event). */
    a->present = 0;
    fake_event(SDL_CONTROLLERDEVICEREMOVED);
    fake_event(SDL_JOYDEVICEREMOVED);
    poll_once();
    assert(a->closed && s_sdl_controllers[0] == NULL && s_sdl_ids[0] == -1);
    assert(s_rumble[0].lo == 0 && s_rumble[0].hi == 0 && s_rumble[0].sent_ms == 0);
    assert(info_status(0) == CELL_PAD_STATUS_CONNECTED);   /* port 0 is the virtual pad: still there */
    assert(s_nevents == 0);                                 /* the events were drained */

    /* Switched on again: the same pad, a new instance id. Port 0 again, motors
     * off, nothing of the old rumble renewed. */
    FakePad* a2 = fake_new(7, 0xA5);
    fake_event(SDL_JOYDEVICEADDED);
    fake_event(SDL_CONTROLLERDEVICEADDED);
    poll_once();
    assert(a2->opens == 1 && s_sdl_controllers[0] == (SDL_GameController*)a2 && s_sdl_ids[0] == 7);
    assert(a2->rumble_calls == 1 && a2->rumble_lo == 0 && a2->rumble_hi == 0);
    a2->buttons = 1u << SDL_CONTROLLER_BUTTON_A;
    assert(data_digital2(0) & CROSS_BIT);                   /* it plays again */
    a2->buttons = 0;
    usleep((PAD_RUMBLE_RENEW_MS + 50) * 1000);
    poll_once();
    assert(a2->rumble_calls == 1);                          /* no old rumble came back */

    /* A second controller with no event (someone else drained the queue):
     * found by the periodic rescan, on port 1, flagged once. */
    FakePad* b = fake_new(9, 0xB6);
    poll_once();
    if (!s_sdl_controllers[1]) { usleep((PAD_SDL_RESCAN_MS + 50) * 1000); poll_once(); }
    assert(b->opens == 1 && s_sdl_controllers[1] == (SDL_GameController*)b);
    assert(a2->opens == 1);                                 /* the open one is not opened twice */
    assert(info_status(1) == (CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES));
    assert(info_status(1) == CELL_PAD_STATUS_CONNECTED);
    b->buttons = 1u << SDL_CONTROLLER_BUTTON_B;
    assert(data_digital2(1) & CIRCLE_BIT);
    b->buttons = 0;

    /* Both go off; the second one comes back first: it gets port 1 back, the
     * port its GUID had, not the lowest free one. */
    a2->present = 0; b->present = 0;
    fake_event(SDL_CONTROLLERDEVICEREMOVED);
    poll_once();
    assert(!s_sdl_controllers[0] && !s_sdl_controllers[1]);
    assert(info_status(1) == (CELL_PAD_STATUS_DISCONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES));
    assert(info_status(1) == CELL_PAD_STATUS_DISCONNECTED);
    FakePad* b2 = fake_new(11, 0xB6);
    fake_event(SDL_CONTROLLERDEVICEADDED);
    poll_once();
    assert(s_sdl_controllers[1] == (SDL_GameController*)b2 && !s_sdl_controllers[0]);
    FakePad* a3 = fake_new(12, 0xA5);
    fake_event(SDL_CONTROLLERDEVICEADDED);
    poll_once();
    assert(s_sdl_controllers[0] == (SDL_GameController*)a3);

    /* A new pad with no history takes the lowest free port. */
    FakePad* c = fake_new(13, 0xC7);
    fake_event(SDL_CONTROLLERDEVICEADDED);
    poll_once();
    assert(s_sdl_controllers[2] == (SDL_GameController*)c);

    assert(cellPadEnd() == CELL_OK);
    assert(a3->closed && b2->closed && c->closed);
    printf("test_pad_hotplug: ok\n");
    return 0;
}

/* Real SDL virtual devices, no GPU or physical-device acceptance. */
#define SDL_MAIN_HANDLED
#define PS3RECOMP_PAD_USE_SDL2 1
#include "../libs/input/cellPad.c"
uint8_t* vm_base;uint32_t ppu_vm_size=0x20000;int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
int spu_coh_is_reserved(uint32_t ea) {(void)ea;return 0;}
void spu_lockline_lock(void) {} void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t ea,uint64_t value,int width) {(void)ea;(void)value;(void)width;}
int ps3_msgdialog_host_input_state(u64* epoch) {*epoch=0;return 0;}
static int checks;
#define CHECK(v) do {++checks;if(!(v)){fprintf(stderr,"FAIL line%d: %s (%s)\n",__LINE__,#v,SDL_GetError());return 1;}} while(0)
static void report(void)
{
    pad_poll_sdl2();s_data_fresh[0]=1;
    atomic_fetch_add_explicit(&s_completed_poll,1,memory_order_release);
    cellPadGetData(0,(CellPadData*)(uintptr_t)0x10000);
}
static u16 buttons(void) {return vm_read16(0x10008)|(vm_read16(0x1000a)<<8);}
int main(void)
{
    static uint8_t memory[0x20000];vm_base=memory;
    SDL_SetMainReady();CHECK(SDL_Init(SDL_INIT_GAMECONTROLLER)==0);
    CHECK(SDL_NumJoysticks()==0); /* Run only on the isolated Linux test host. */
    CHECK(SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_UNKNOWN,3,0,0)==0);
    SDL_VirtualJoystickDesc desc={0};desc.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type=SDL_JOYSTICK_TYPE_GAMECONTROLLER;desc.naxes=6;desc.nbuttons=15;
    desc.button_mask=0x7fff;desc.axis_mask=0x3f;desc.name="Folklore fixture pad";
    CHECK(SDL_JoystickAttachVirtualEx(&desc)==1);
    CHECK(SDL_JoystickAttachVirtualEx(&desc)==2);
    SDL_Joystick* a=SDL_JoystickOpen(1);SDL_Joystick* b=SDL_JoystickOpen(2);
    CHECK(a && b);CHECK(cellPadInit(2)==0);
    CHECK(SDL_JoystickSetVirtualButton(a,SDL_CONTROLLER_BUTTON_A,1)==0);
    CHECK(SDL_JoystickSetVirtualButton(b,SDL_CONTROLLER_BUTTON_B,1)==0);
    report();CHECK(buttons()&CELL_PAD_CTRL_CROSS);
    CHECK(s_host_state[1].buttons&CELL_PAD_CTRL_CIRCLE);
    CHECK(s_sdl_instance[0]!=s_sdl_instance[1]);
    SDL_JoystickID bid=SDL_JoystickInstanceID(b);
    SDL_JoystickClose(a);CHECK(SDL_JoystickDetachVirtual(1)==0);
    report();CHECK(buttons()&CELL_PAD_CTRL_CIRCLE);
    CHECK(!(buttons()&CELL_PAD_CTRL_CROSS));CHECK(s_sdl_instance[0]==bid);
    CHECK(!s_host_state[1].connected && !s_sdl_controllers[1]);
    CHECK(SDL_JoystickSetVirtualAxis(b,SDL_CONTROLLER_AXIS_LEFTX,32767)==0);
    report();CHECK(vm_read16(0x10004+CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X*2)==255);
    CHECK(SDL_JoystickDetachVirtual(0)==0);
    report();CHECK(s_sdl_instance[0]==bid && (buttons()&CELL_PAD_CTRL_CIRCLE));
    CHECK(cellPadEnd()==0);SDL_JoystickClose(b);CHECK(SDL_JoystickDetachVirtual(0)==0);
    SDL_Quit();printf("SDL virtual gamepad guest ABI: %d checks passed\n",checks);return 0;
}

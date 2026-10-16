/* Linux actual SDL event mapping -> atomic bridge -> cellPad guest reports.
 * All device reads and window events here are synthetic CPU test fixtures. */
#include "pad_sdl_fixture.h"
#include "../libs/input/pad_keyboard_sdl_events.h"
#include "../libs/input/pad_keyboard_cache.h"

static void key(Uint32 id,SDL_Scancode code,int down,int repeat)
{
    SDL_Event e={0};e.type=down?SDL_KEYDOWN:SDL_KEYUP;
    e.key.windowID=id;e.key.keysym.scancode=code;e.key.repeat=(Uint8)repeat;
    ps3_pad_keyboard_sdl_event(&e,7);
}
static void focus(Uint32 id,int on)
{
    SDL_Event e={0};e.type=SDL_WINDOWEVENT;e.window.windowID=id;
    e.window.event=on?SDL_WINDOWEVENT_FOCUS_GAINED:SDL_WINDOWEVENT_FOCUS_LOST;
    ps3_pad_keyboard_sdl_event(&e,7);
}
static void keyboard_report(void)
{
    pad_backend_lock();pad_poll_sdl2();
    if(!s_host_state[0].connected)pad_poll_window_keyboard();
    s_data_fresh[0]=1;
    atomic_fetch_add_explicit(&s_completed_poll,1,memory_order_release);pad_backend_unlock();
    CHECK(cellPadGetData(0,(CellPadData*)(uintptr_t)0x10000)==0);
}
static u16 buttons(void) { return word(2)|(word(3)<<8); }
int main(int argc,char** argv)
{
    (void)argv;
    static uint8_t memory[0x20000];vm_base=memory;fixture.attached=0;
    CHECK(cellPadInit(1)==0);
    ps3_pad_keyboard_attach(7);focus(7,1);
    if(argc>1) {
        key(7,SDL_SCANCODE_Z,1,0);keyboard_report();
        CHECK(buttons()==0);CHECK(cellPadEnd()==0);
        printf("keyboard disabled: %d checks, %d failures\n",checks,failures);return failures?1:0;
    }
    keyboard_report();CHECK(buttons()==0&&length()==8);
    CHECK(cellPadInfoSensorMode(0)==0);
    CHECK((u32)cellPadSetSensorMode(0,1)==CELL_PAD_ERROR_UNSUPPORTED_GAMEPAD);
    CHECK(cellPadSetPortSetting(0,CELL_PAD_SETTING_PRESS_ON)==0);
    static const struct {SDL_Scancode key;u16 button;} map[]={
        {SDL_SCANCODE_UP,CELL_PAD_CTRL_UP},{SDL_SCANCODE_DOWN,CELL_PAD_CTRL_DOWN},
        {SDL_SCANCODE_LEFT,CELL_PAD_CTRL_LEFT},{SDL_SCANCODE_RIGHT,CELL_PAD_CTRL_RIGHT},
        {SDL_SCANCODE_Z,CELL_PAD_CTRL_CROSS},{SDL_SCANCODE_X,CELL_PAD_CTRL_CIRCLE},
        {SDL_SCANCODE_A,CELL_PAD_CTRL_SQUARE},{SDL_SCANCODE_S,CELL_PAD_CTRL_TRIANGLE},
        {SDL_SCANCODE_Q,CELL_PAD_CTRL_L1},{SDL_SCANCODE_W,CELL_PAD_CTRL_R1},
        {SDL_SCANCODE_1,CELL_PAD_CTRL_L2},{SDL_SCANCODE_2,CELL_PAD_CTRL_R2},
        {SDL_SCANCODE_RETURN,CELL_PAD_CTRL_START},{SDL_SCANCODE_TAB,CELL_PAD_CTRL_SELECT}};
    for(unsigned i=0;i<sizeof(map)/sizeof(map[0]);++i) {
        key(7,map[i].key,1,0);keyboard_report();CHECK(buttons()==map[i].button);
        key(7,map[i].key,0,0);keyboard_report();CHECK(buttons()==0);
    }
    key(7,SDL_SCANCODE_LEFT,1,0);key(7,SDL_SCANCODE_UP,1,0);key(7,SDL_SCANCODE_Z,1,0);
    keyboard_report();CHECK(word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X)==0&&word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y)==0);
    CHECK(word(CELL_PAD_BTN_OFFSET_PRESS_CROSS)==255&&word(20)==0);
    focus(8,0);key(8,SDL_SCANCODE_Z,0,0);keyboard_report();CHECK(buttons()&CELL_PAD_CTRL_CROSS);
    focus(7,0);keyboard_report();CHECK(buttons()==0&&word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X)==128);
    focus(7,1);key(7,SDL_SCANCODE_Z,1,1);keyboard_report();CHECK(buttons()==0);
    key(7,SDL_SCANCODE_Z,0,0);key(7,SDL_SCANCODE_Z,1,0);keyboard_report();CHECK(buttons()==CELL_PAD_CTRL_CROSS);
    dialog=1;epoch=1;keyboard_report();CHECK(buttons()==0);
    dialog=0;keyboard_report();CHECK(buttons()==0);
    key(7,SDL_SCANCODE_Z,0,0);keyboard_report();keyboard_report();
    key(7,SDL_SCANCODE_Z,1,0);keyboard_report();CHECK(buttons()==CELL_PAD_CTRL_CROSS);
    /* A real controller wins over the keyboard and does not gain motion from it. */
    fixture.attached=1;read_pad();CHECK(buttons()==0);
    fixture.attached=0;keyboard_report();CHECK(buttons()==CELL_PAD_CTRL_CROSS);
    ps3_pad_keyboard_detach();keyboard_report();CHECK(buttons()==0);
    ps3_pad_keyboard_attach(9);focus(7,1);key(7,SDL_SCANCODE_Z,1,0);keyboard_report();CHECK(buttons()==0);
    focus(9,1);key(9,SDL_SCANCODE_RETURN,1,0);keyboard_report();CHECK(buttons()==CELL_PAD_CTRL_START);
    for(unsigned i=0;i<3;++i) {
        SDL_Event e={0};e.type=SDL_WINDOWEVENT;e.window.windowID=9;
        e.window.event=i==0?SDL_WINDOWEVENT_HIDDEN:i==1?SDL_WINDOWEVENT_MINIMIZED:SDL_WINDOWEVENT_CLOSE;
        ps3_pad_keyboard_sdl_event(&e,9);keyboard_report();CHECK(buttons()==0);
        focus(9,1);key(9,SDL_SCANCODE_RETURN,1,0);keyboard_report();CHECK(buttons()==CELL_PAD_CTRL_START);
    }
    SDL_Event background={0};background.type=SDL_APP_WILLENTERBACKGROUND;
    ps3_pad_keyboard_sdl_event(&background,9);keyboard_report();CHECK(buttons()==0);
    focus(9,1);key(9,SDL_SCANCODE_RETURN,1,0);
    SDL_Event quit={0};quit.type=SDL_QUIT;ps3_pad_keyboard_sdl_event(&quit,9);
    keyboard_report();CHECK(buttons()==0&&ps3_pad_keyboard_read()==0);
    CHECK(cellPadEnd()==0);
    printf("Linux keyboard guest integration: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}

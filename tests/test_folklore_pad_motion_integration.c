/* CPU-only guest ABI regression. Every SDL controller call uses this fixture. */
#define SDL_MAIN_HANDLED
#define PS3RECOMP_PAD_USE_SDL2 1
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static ULONGLONG fake_ms=100;
static ULONGLONG fake_time(void) { return fake_ms; }
static HWND fake_foreground(void) { return NULL; }
#define GetTickCount64 fake_time
#define GetForegroundWindow fake_foreground
#endif
typedef struct { int attached,capable,on[2],read_fail,disable_fail; Uint64 ts; float accel[3],gyro[3]; } Fixture;
static Fixture fixture={1,1,{0,0},0,0,1234,{0,9.80665f,0},{0,1.57079632679f,0}};
static SDL_GameController* controller(void) { return (SDL_GameController*)(void*)&fixture; }
static int sensor_index(SDL_SensorType t) { return t==SDL_SENSOR_GYRO; }
static SDL_bool attached(SDL_GameController* p) { return p==controller() && fixture.attached; }
static SDL_bool capable(SDL_GameController* p,SDL_SensorType t) { (void)t;return attached(p)&&fixture.capable; }
static int enable(SDL_GameController* p,SDL_SensorType t,SDL_bool on)
{ if(!capable(p,t)||(!on&&fixture.disable_fail))return -1;fixture.on[sensor_index(t)]=on;return 0; }
static SDL_bool fixture_enabled(SDL_GameController* p,SDL_SensorType t)
{ return capable(p,t)&&fixture.on[sensor_index(t)]; }
static int sensor(SDL_GameController* p,SDL_SensorType t,Uint64* ts,float* values,int n)
{ if(!fixture_enabled(p,t)||n!=3||fixture.read_fail)return -1;*ts=fixture.ts;memcpy(values,t==SDL_SENSOR_GYRO?fixture.gyro:fixture.accel,3*sizeof(float));return 0; }
static void update(void) {}
static SDL_bool is_controller(int i) { return i==0&&fixture.attached; }
static SDL_GameController* open_controller(int i) { return is_controller(i)?controller():NULL; }
static void close_controller(SDL_GameController* p) { (void)p; }
static Uint8 button(SDL_GameController* p,SDL_GameControllerButton b) { (void)p;(void)b;return 0; }
static Sint16 axis(SDL_GameController* p,SDL_GameControllerAxis a) { (void)p;(void)a;return 0; }
static Uint32 was_init(Uint32 flags) { return flags; }
static int init(Uint32 flags) { (void)flags;return 0; }
static int joysticks(void) { return fixture.attached; }
static int rumble(SDL_GameController* p,Uint16 a,Uint16 b,Uint32 t) { (void)p;(void)a;(void)b;(void)t;return 0; }
#define SDL_GameControllerGetAttached attached
#define SDL_GameControllerHasSensor capable
#define SDL_GameControllerSetSensorEnabled enable
#define SDL_GameControllerIsSensorEnabled fixture_enabled
#define SDL_GameControllerGetSensorDataWithTimestamp sensor
#define SDL_GameControllerUpdate update
#define SDL_IsGameController is_controller
#define SDL_GameControllerOpen open_controller
#define SDL_GameControllerClose close_controller
#define SDL_GameControllerGetButton button
#define SDL_GameControllerGetAxis axis
#define SDL_WasInit was_init
#define SDL_InitSubSystem init
#define SDL_NumJoysticks joysticks
#define SDL_GameControllerRumble rumble
#include "../libs/input/cellPad.c"
uint8_t* vm_base;
uint32_t ppu_vm_size=0x20000;
int g_resv_store_active;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
uint32_t g_ww_lo,g_ww_hi;
int spu_coh_is_reserved(uint32_t ea) { (void)ea;return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea,uint64_t value,int width) { (void)ea;(void)value;(void)width; }
static int dialog;
static u64 epoch;
int ps3_msgdialog_host_input_state(u64* e) { *e=epoch;return dialog; }
static int checks,failures;
#define CHECK(x) do { ++checks;if(!(x)){++failures;fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);} } while(0)
static void read_pad(void)
{
    /* Explicit mock sweep makes this CPU test independent of wall-clock pacing. */
    pad_backend_lock();pad_poll_sdl2();s_data_fresh[0]=1;
    atomic_fetch_add_explicit(&s_completed_poll,1,memory_order_release);pad_backend_unlock();
    CHECK(cellPadGetData(0,(CellPadData*)(uintptr_t)0x10000)==0);
}
static u16 word(unsigned i) { return vm_read16(0x10004+i*2); }
static u32 length(void) { return vm_read32(0x10000); }
int main(void)
{
    static uint8_t memory[0x20000];vm_base=memory;
    CHECK(cellPadInit(1)==0);
    CHECK(cellPadInfoSensorMode(0)==1);
    CHECK(cellPadGetInfo2((CellPadInfo2*)(uintptr_t)0x11000)==0);
    CHECK(vm_read32(0x11000+offsetof(CellPadInfo2,device_capability))&CELL_PAD_CAPABILITY_SENSOR_MODE);
    CHECK(cellPadPeriphGetInfo((CellPadPeriphInfo*)(uintptr_t)0x12000)==0);
    CHECK(vm_read32(0x12000+offsetof(CellPadPeriphInfo,device_capability))&CELL_PAD_CAPABILITY_SENSOR_MODE);
    CHECK(cellPadGetCapabilityInfo(0,(CellPadCapabilityInfo*)(uintptr_t)0x13000)==0);
    CHECK(vm_read32(0x13000)&CELL_PAD_CAPABILITY_SENSOR_MODE);
    read_pad();CHECK(length()==8&&word(20)==0);
    CHECK(word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X)==128);
    CHECK(pad_sdl_axis_to_u8(-32768)==0&&pad_sdl_axis_to_u8(32767)==255);
    CHECK(cellPadSetSensorMode(0,1)==0);
    read_pad();CHECK(length()==24&&word(1)==0x7c);
    CHECK(word(20)==512&&word(21)==399&&word(22)==512&&word(23)==635);
    CHECK(memory[0x10004+21*2]==1&&memory[0x10005+21*2]==0x8f);
    fixture.read_fail=1;read_pad();CHECK(length()==8&&word(20)==0&&word(23)==0);
    fixture.read_fail=0;fixture.ts=0;read_pad();CHECK(length()==8&&word(21)==0);
    fixture.ts=1234;read_pad();CHECK(length()==24&&word(21)==399);
    dialog=1;epoch=1;read_pad();CHECK(length()==0&&word(21)==0);
    dialog=0;read_pad();read_pad();CHECK(length()==24&&word(21)==399);
    CHECK(cellPadSetSensorMode(0,0)==0);read_pad();CHECK(length()==8&&word(20)==0);
    CHECK(cellPadSetPortSetting(0,CELL_PAD_SETTING_SENSOR_ON|CELL_PAD_SETTING_PRESS_ON)==0);
    read_pad();CHECK(length()==24&&word(23)==635);
    fixture.disable_fail=1;
    CHECK((u32)cellPadSetSensorMode(0,0)==CELL_PAD_ERROR_UNSUPPORTED_GAMEPAD);
    read_pad();CHECK(length()==20&&word(20)==0);
    CHECK(!(s_port_setting[0]&CELL_PAD_SETTING_SENSOR_ON)&&!s_sdl_motion[0].enabled);
    fixture.disable_fail=0;CHECK(cellPadSetSensorMode(0,1)==0);
    fixture.disable_fail=1;
    CHECK((u32)cellPadSetPortSetting(0,CELL_PAD_SETTING_PRESS_ON)==CELL_PAD_ERROR_UNSUPPORTED_GAMEPAD);
    read_pad();CHECK(length()==20&&word(21)==0);
    CHECK(!(s_port_setting[0]&CELL_PAD_SETTING_SENSOR_ON));
    fixture.disable_fail=0;CHECK(cellPadSetSensorMode(0,1)==0);
    fixture.attached=0;read_pad();CHECK(length()!=24&&word(20)==0);
    CHECK(word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X)==128&&word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y)==128);
    CHECK(cellPadInfoSensorMode(0)!=1);
    CHECK(cellPadGetCapabilityInfo(0,(CellPadCapabilityInfo*)(uintptr_t)0x13000)==0);
    CHECK(!(vm_read32(0x13000)&CELL_PAD_CAPABILITY_SENSOR_MODE));
    fixture.attached=1;fixture.gyro[1]=-1.57079632679f;read_pad();CHECK(length()==24&&word(23)==389);
    fixture.capable=0;read_pad();CHECK(length()==20&&word(23)==0);
    CHECK(cellPadInfoSensorMode(0)==0);
    CHECK((u32)cellPadSetSensorMode(0,1)==CELL_PAD_ERROR_UNSUPPORTED_GAMEPAD);
    CHECK(cellPadEnd()==0);
    printf("pad motion guest integration: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}

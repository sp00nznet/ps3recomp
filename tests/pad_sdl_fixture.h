#ifndef FOLKLORE_PAD_SDL_FIXTURE_H
#define FOLKLORE_PAD_SDL_FIXTURE_H
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
static Fixture fixture_second;
static SDL_GameController* controller(void) { return (SDL_GameController*)(void*)&fixture; }
static SDL_GameController* second_controller(void) { return (SDL_GameController*)(void*)&fixture_second; }
static int sensor_index(SDL_SensorType t) { return t==SDL_SENSOR_GYRO; }
static SDL_bool attached(SDL_GameController* p) { return p==controller()?fixture.attached:p==second_controller()&&fixture_second.attached; }
static SDL_bool capable(SDL_GameController* p,SDL_SensorType t) { (void)t;return attached(p)&&(p==controller()?fixture.capable:fixture_second.capable); }
static int enable(SDL_GameController* p,SDL_SensorType t,SDL_bool on)
{ if(!capable(p,t)||(!on&&fixture.disable_fail))return -1;fixture.on[sensor_index(t)]=on;return 0; }
static SDL_bool fixture_enabled(SDL_GameController* p,SDL_SensorType t)
{ return capable(p,t)&&fixture.on[sensor_index(t)]; }
static int sensor(SDL_GameController* p,SDL_SensorType t,Uint64* ts,float* values,int n)
{ if(!fixture_enabled(p,t)||n!=3||fixture.read_fail)return -1;*ts=fixture.ts;memcpy(values,t==SDL_SENSOR_GYRO?fixture.gyro:fixture.accel,3*sizeof(float));return 0; }
static void update(void) {}
static int leading_unmapped, fixture_opens, fixture_closes, fixture_open_fail;
static SDL_JoystickID fixture_instance = 42;
static SDL_JoystickID fixture_second_instance = 84;
static SDL_bool is_controller(int i) { return (i==leading_unmapped&&fixture.attached)||(i==leading_unmapped+fixture.attached&&fixture_second.attached); }
static SDL_JoystickID device_instance(int i) { return !is_controller(i)?-1:i==leading_unmapped&&fixture.attached?fixture_instance:fixture_second_instance; }
static SDL_GameController* open_controller(int i) { if(!is_controller(i)||fixture_open_fail)return NULL; ++fixture_opens;return i==leading_unmapped&&fixture.attached?controller():second_controller(); }
static void close_controller(SDL_GameController* p) { (void)p;++fixture_closes; }
static uint32_t fixture_buttons;
static Sint16 fixture_axes[SDL_CONTROLLER_AXIS_MAX];
static Uint8 button(SDL_GameController* p,SDL_GameControllerButton b) { (void)p;return !!(fixture_buttons & (1u << b)); }
static Sint16 axis(SDL_GameController* p,SDL_GameControllerAxis a) { (void)p;return fixture_axes[a]; }
static Uint32 was_init(Uint32 flags) { return flags; }
static int init(Uint32 flags) { (void)flags;return 0; }
static int joysticks(void) { return leading_unmapped+fixture.attached+fixture_second.attached; }
static int rumble(SDL_GameController* p,Uint16 a,Uint16 b,Uint32 t) { (void)p;(void)a;(void)b;(void)t;return 0; }
static Uint16 fixture_vid,fixture_pid;
static Uint16 fixture_second_vid,fixture_second_pid;
static Uint16 fixture_vendor(SDL_GameController* p) { return p==second_controller()?fixture_second_vid:fixture_vid; }
static Uint16 fixture_product(SDL_GameController* p) { return p==second_controller()?fixture_second_pid:fixture_pid; }
#ifdef PAD_FIXTURE_G7_MOCK
#include "../libs/input/g7_motion_backend.h"
static Uint64 fixture_ticks=100;
static Uint64 fixture_get_ticks(void) { return fixture_ticks; }
#define SDL_GetTicks64 fixture_get_ticks
static int fixture_g7_starts,fixture_g7_stops,fixture_g7_services;
static int fixture_g7_idle=1,fixture_g7_start_ok=1,fixture_g7_hold_stop;
static Uint16 fixture_g7_started_vid,fixture_g7_started_pid;
static g7_motion_sample fixture_g7_raw;
static g7_motion_state fixture_g7_start_state=G7_MOTION_RUNNING;
static int fixture_g7_start(uint16_t vid,uint16_t pid) {
    ++fixture_g7_starts;
    fixture_g7_started_vid=vid;fixture_g7_started_pid=pid;
    if(!fixture_g7_idle||!fixture_g7_start_ok)return 0;
    fixture_g7_idle=0;
    fixture_g7_raw.state=fixture_g7_start_state;
    return 1;
}
static void fixture_g7_stop(void) {
    ++fixture_g7_stops;
    memset(&fixture_g7_raw,0,sizeof(fixture_g7_raw));
    fixture_g7_raw.state=G7_MOTION_STOPPED;
    if(!fixture_g7_hold_stop)fixture_g7_idle=1;
}
static int fixture_g7_service(void) { ++fixture_g7_services;return fixture_g7_idle; }
static int fixture_g7_sample(g7_motion_sample *out) {
    *out=fixture_g7_raw;
    return out->fresh_valid&&out->state==G7_MOTION_RUNNING;
}
#define g7_motion_start fixture_g7_start
#define g7_motion_stop fixture_g7_stop
#define g7_motion_service fixture_g7_service
#define g7_motion_get_raw_sample fixture_g7_sample
#endif
#ifdef PAD_FIXTURE_DS3_MOCK
#include "ds3_motion_backend.h"
static int fixture_ds3_starts,fixture_ds3_stops,fixture_ds3_services;
static int fixture_ds3_idle=1,fixture_ds3_start_ok=1,fixture_ds3_hold_stop;
static Uint16 fixture_ds3_started_vid,fixture_ds3_started_pid;
static ds3_motion_sample fixture_ds3_raw;
static ds3_motion_state fixture_ds3_start_state=DS3_MOTION_RUNNING;
static int fixture_ds3_start(uint16_t vid,uint16_t pid) {
    ++fixture_ds3_starts;fixture_ds3_started_vid=vid;fixture_ds3_started_pid=pid;
    if(!fixture_ds3_idle||!fixture_ds3_start_ok)return 0;
    fixture_ds3_idle=0;fixture_ds3_raw.state=fixture_ds3_start_state;return 1;
}
static void fixture_ds3_stop(void) {
    ++fixture_ds3_stops;memset(&fixture_ds3_raw,0,sizeof(fixture_ds3_raw));
    fixture_ds3_raw.state=DS3_MOTION_STOPPED;
    if(!fixture_ds3_hold_stop)fixture_ds3_idle=1;
}
static int fixture_ds3_service(void) { ++fixture_ds3_services;return fixture_ds3_idle; }
static int fixture_ds3_sample(ds3_motion_sample *out) {
    *out=fixture_ds3_raw;return out->fresh_valid&&out->state==DS3_MOTION_RUNNING;
}
#define ds3_motion_start fixture_ds3_start
#define ds3_motion_stop fixture_ds3_stop
#define ds3_motion_service fixture_ds3_service
#define ds3_motion_get_sample fixture_ds3_sample
#endif
#define SDL_GameControllerGetVendor fixture_vendor
#define SDL_GameControllerGetProduct fixture_product
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
#define SDL_JoystickGetDeviceInstanceID device_instance
#define SDL_LockJoysticks update
#define SDL_UnlockJoysticks update
#define SDL_GameControllerRumble rumble
#ifndef PAD_FIXTURE_CELL_PAD_SOURCE
#define PAD_FIXTURE_CELL_PAD_SOURCE "../libs/input/cellPad.c"
#endif
#include PAD_FIXTURE_CELL_PAD_SOURCE
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
#endif

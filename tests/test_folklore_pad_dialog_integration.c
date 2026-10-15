/* Windows CPU fixture: actual cellPad report packing, simulated XInput. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static ULONGLONG now_ms=100;
static XINPUT_GAMEPAD pad;
static ULONGLONG fixture_time(void) { return now_ms; }
static DWORD fixture_xinput(DWORD port,XINPUT_STATE* state)
{
    memset(state,0,sizeof(*state));state->Gamepad=pad;
    return port==0?ERROR_SUCCESS:ERROR_DEVICE_NOT_CONNECTED;
}
static DWORD fixture_vibration(DWORD port,XINPUT_VIBRATION* state)
{ (void)port;(void)state;return ERROR_SUCCESS; }
static HWND fixture_foreground(void) { return NULL; }
#define GetTickCount64 fixture_time
#define XInputGetState fixture_xinput
#define XInputSetState fixture_vibration
#define GetForegroundWindow fixture_foreground
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
void ps3_ww_report_inline(uint32_t ea,uint64_t value,int width)
{ (void)ea;(void)value;(void)width; }
static u64 input_epoch;
static int input_active;
static int interleave;
static HANDLE poll_started,poll_done,poll_thread;
static void check(int ok,const char* label);
static DWORD WINAPI concurrent_poll(void* unused)
{
    (void)unused;SetEvent(poll_started);pad_poll_backend();SetEvent(poll_done);return 0;
}
int ps3_msgdialog_host_input_state(u64* epoch)
{
    if(interleave) {
        interleave=0;pad.wButtons=XINPUT_GAMEPAD_START;now_ms+=5;
        poll_thread=CreateThread(NULL,0,concurrent_poll,NULL,0,NULL);
        check(poll_thread && WaitForSingleObject(poll_started,1000)==WAIT_OBJECT_0,
              "concurrent poll reaches sampling lock");
        check(WaitForSingleObject(poll_done,20)==WAIT_TIMEOUT,
              "new poll cannot publish inside raw-serial filter snapshot");
    }
    *epoch=input_epoch;return input_active;
}
static int errors;
static void check(int ok,const char* label)
{ if(!ok){++errors;fprintf(stderr,"FAIL: %s\n",label);} }
static void read_pad(void)
{ check(cellPadGetData(0,(CellPadData*)(uintptr_t)0x10000)==0,"actual GetData succeeds"); }
static u16 word(unsigned index) { return vm_read16(0x10004+index*2); }
static void fresh(void) { now_ms+=5;read_pad(); }

int main(void)
{
    uint8_t memory[0x20000]={0};vm_base=memory;
    check(cellPadInit(1)==0,"pad initializes");
    check(cellPadSetPortSetting(0,CELL_PAD_SETTING_PRESS_ON|CELL_PAD_SETTING_SENSOR_ON)==0,
          "pressure and sensors enabled");
    pad.wButtons=XINPUT_GAMEPAD_START;fresh();
    check(word(CELL_PAD_BTN_OFFSET_DIGITAL1)&CELL_PAD_CTRL_START,"ordinary START reaches guest");
    input_epoch=1;input_active=1;fresh();
    check(word(CELL_PAD_BTN_OFFSET_DIGITAL1)==0 && word(CELL_PAD_BTN_OFFSET_DIGITAL2)==0,
          "active dialog blocks actual packed digital report");
    check(word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X)==128 &&
          word(CELL_PAD_BTN_OFFSET_SENSOR_X)==512,"neutral stick and sensor rest retained");
    input_active=0;fresh();
    check(word(CELL_PAD_BTN_OFFSET_DIGITAL1)==0,"held activation key remains blocked");
    pad.wButtons=0;pad.bLeftTrigger=8;pad.bRightTrigger=7;fresh();
    pad.wButtons=XINPUT_GAMEPAD_START;fresh();
    check(word(CELL_PAD_BTN_OFFSET_DIGITAL1)&CELL_PAD_CTRL_START,
          "PRESS_ON resting trigger noise releases quarantine before new press");
    input_epoch=2;input_active=0;read_pad();
    check(word(CELL_PAD_BTN_OFFSET_DIGITAL1)==0,"unseen closed dialog epoch quarantines cached report");
    pad.wButtons=0;fresh();
    pad.wButtons=XINPUT_GAMEPAD_A;fresh();
    check(word(CELL_PAD_BTN_OFFSET_DIGITAL2)!=0 && word(CELL_PAD_BTN_OFFSET_PRESS_CROSS)==255,
          "new input and full pressure reach guest after release");
    pad.wButtons=0;pad.bLeftTrigger=pad.bRightTrigger=0;fresh();
    poll_started=CreateEventW(NULL,TRUE,FALSE,NULL);
    poll_done=CreateEventW(NULL,TRUE,FALSE,NULL);
    check(poll_started && poll_done,"interleaving events created");
    input_epoch=3;interleave=1;read_pad();
    check(word(CELL_PAD_BTN_OFFSET_DIGITAL1)==0,"new epoch suppresses stale neutral report");
    check(poll_thread && WaitForSingleObject(poll_thread,1000)==WAIT_OBJECT_0 &&
          WaitForSingleObject(poll_done,0)==WAIT_OBJECT_0,"blocked poll completes after snapshot unlock");
    fresh();
    check(word(CELL_PAD_BTN_OFFSET_DIGITAL1)==0,"later held poll cannot escape quarantine");
    if(poll_thread)CloseHandle(poll_thread);
    if(poll_started)CloseHandle(poll_started);
    if(poll_done)CloseHandle(poll_done);
    cellPadEnd();
    printf("pad dialog report integration: %s\n",errors?"FAIL":"PASS");
    return errors?1:0;
}

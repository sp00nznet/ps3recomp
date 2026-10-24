/* Actual cellPadGetData + BE guest serialization, with deterministic host time
 * and fake SDL devices. No game process or physical input is read. */
#include <time.h>
#include <stdint.h>
static uint64_t fake_ms=1000;
static int fixture_clock_gettime(clockid_t id,struct timespec* ts)
{ (void)id;ts->tv_sec=fake_ms/1000;ts->tv_nsec=(fake_ms%1000)*1000000;return 0; }
#define clock_gettime fixture_clock_gettime
#include "../cellPad.c"
#undef clock_gettime
#include <assert.h>
#include <sys/mman.h>
#include <sys/stat.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi;
void ppu_resv_break_store(uint64_t a){(void)a;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
int spu_coh_is_reserved(uint32_t a){(void)a;return 0;}
void spu_coh_notify_write(uint32_t a){(void)a;}
void spu_lockline_lock(void){}
void spu_lockline_unlock(void){}
u32 cellGcm_flip_request_count(void){return 0;}

static int device,attached;
static u16 device_buttons;
void SDL_GameControllerUpdate(void){}
SDL_bool SDL_IsGameController(int n){(void)n;return SDL_FALSE;}
SDL_bool SDL_GameControllerGetAttached(SDL_GameController* gc)
{return gc==(SDL_GameController*)&device && attached;}
void SDL_GameControllerClose(SDL_GameController* gc){(void)gc;}
int SDL_GameControllerRumble(SDL_GameController* gc,Uint16 lo,Uint16 hi,Uint32 ms)
{(void)gc;(void)lo;(void)hi;(void)ms;return 0;}
Uint8 SDL_GameControllerGetButton(SDL_GameController* gc,SDL_GameControllerButton b)
{
    (void)gc;
    if(b==SDL_CONTROLLER_BUTTON_B)return !!(device_buttons&CELL_PAD_CTRL_CIRCLE);
    if(b==SDL_CONTROLLER_BUTTON_A)return !!(device_buttons&CELL_PAD_CTRL_CROSS);
    return 0;
}
Sint16 SDL_GameControllerGetAxis(SDL_GameController* gc,SDL_GameControllerAxis a)
{(void)gc;return a==SDL_CONTROLLER_AXIS_TRIGGERLEFT||a==SDL_CONTROLLER_AXIS_TRIGGERRIGHT?0:1;}

static CellPadData* guest=(CellPadData*)(uintptr_t)0x100;
static void publish(const char* path,const char* text)
{FILE* f=fopen(path,"w");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static off_t file_size(const char* path)
{struct stat s;assert(!stat(path,&s));return s.st_size;}
static s32 read_pad(unsigned port)
{assert(cellPadGetData(port,guest)==CELL_OK);return (s32)vm_read32(0x100);}
static u16 word(unsigned n){return vm_read16(0x104+n*2);}
static u16 buttons(void)
{return word(CELL_PAD_BTN_OFFSET_DIGITAL1)|(word(CELL_PAD_BTN_OFFSET_DIGITAL2)<<8);}
static void next_report(void){fake_ms+=PAD_POLL_INTERVAL_MS;}
static void drains(unsigned count)
{for(unsigned n=0;n<count;n++)assert(read_pad(0)==0);}
static void held_reports(unsigned n,u16 held,u16 after)
{
    for(unsigned k=0;k<n;k++){
        next_report();assert(read_pad(0)>0);assert(buttons()==held);
        drains(100);
    }
    next_report();assert(read_pad(0)>0);assert(buttons()==after);
}
int main(int argc,char** argv)
{
    assert(argc==3);const char* mode=argv[1],*path=argv[2];
    if(strcmp(mode,"other-inputs"))setenv("PAD_FILE",path,1);
    else{setenv("PAD_SCRIPT","0:0x0020",1);setenv("PAD_STICK","140,150,160,170",1);}
    if(!strcmp(mode,"always"))setenv("PAD_ALWAYS_CHANGE","1",1);
    vm_base=mmap(NULL,0x10000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(vm_base!=MAP_FAILED);ppu_vm_size=0x10000;
    publish(path,"");s_pad_initialized=1;s_max_connect=7;
    /* First call takes the host report. Admission on subsequent same-tick
     * reads is allowed, but these are len0 and must not spend a hold. */
    assert(read_pad(0)>0);
    if(!strcmp(mode,"drains")){
        publish(path,"0x4000 4\n");assert(read_pad(0)==0);
        assert(!file_size(path)); /* admission ACK is not packet consumption */
        drains(200);held_reports(4,CELL_PAD_CTRL_CROSS,0);
    }else if(!strcmp(mode,"default")){
        publish(path,"0x4000\n");assert(read_pad(0)==0);drains(100);
        held_reports(40,CELL_PAD_CTRL_CROSS,0);
    }else if(!strcmp(mode,"ports-clear")){
        publish(path,"0x4000 2\n");
        assert(cellPadGetData(7,guest)==(s32)CELL_PAD_ERROR_INVALID_PARAMETER);
        assert(cellPadGetData(0,NULL)==(s32)CELL_PAD_ERROR_INVALID_PARAMETER);
        s_pad_initialized=0;assert(cellPadGetData(0,guest)==(s32)CELL_PAD_ERROR_NOT_OPENED);
        s_pad_initialized=1;assert(file_size(path)>0);
        assert(read_pad(1)==0 && file_size(path)>0); /* disconnected nonzero port */
        assert(cellPadClearBuf(0)==CELL_OK);
        assert(cellPadClearBuf(7)==(s32)CELL_PAD_ERROR_INVALID_PARAMETER);
        assert(read_pad(0)==0 && !file_size(path));
        assert(cellPadClearBuf(0)==CELL_OK);
        for(unsigned n=0;n<100;n++)assert(read_pad(1)==0);
        held_reports(2,CELL_PAD_CTRL_CROSS,0);
    }else if(!strcmp(mode,"physical")){
        s_sdl_controllers[0]=(SDL_GameController*)&device;attached=1;
        device_buttons=CELL_PAD_CTRL_CIRCLE;
        publish(path,"0x4000 2\n");assert(read_pad(0)==0);
        held_reports(2,CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE,CELL_PAD_CTRL_CIRCLE);
        device_buttons=0;next_report();assert(read_pad(0)>0 && !buttons());
        attached=0;next_report();assert(read_pad(0)>0 && !buttons());
    }else if(!strcmp(mode,"pressure-sensor")){
        s_port_setting[0]=CELL_PAD_SETTING_PRESS_ON|CELL_PAD_SETTING_SENSOR_ON;
        publish(path,"0x4000 2\n");assert(read_pad(0)==0);drains(100);
        for(unsigned n=0;n<2;n++){
            next_report();assert(read_pad(0)==CELL_PAD_LEN_CHANGE_SENSOR_ON);
            assert(word(0)==0 && word(1)==0x7E);
            assert(buttons()==CELL_PAD_CTRL_CROSS);
            assert(word(CELL_PAD_BTN_OFFSET_PRESS_CROSS)==255);
            assert(word(CELL_PAD_BTN_OFFSET_SENSOR_X)==512);
            drains(100);
        }
        next_report();assert(read_pad(0)==CELL_PAD_LEN_CHANGE_SENSOR_ON);
        assert(!buttons() && word(CELL_PAD_BTN_OFFSET_PRESS_CROSS)==0);
    }else if(!strcmp(mode,"always")){
        publish(path,"0x4000 3\n");
        for(unsigned n=0;n<3;n++)assert(read_pad(0)>0 && buttons()==CELL_PAD_CTRL_CROSS);
        assert(read_pad(0)>0 && !buttons());
    }else if(!strcmp(mode,"other-inputs")){
        next_report();assert(read_pad(0)>0 && buttons()==CELL_PAD_CTRL_RIGHT);
        assert(word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X)==140);
        assert(word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y)==150);
        assert(word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X)==160);
        assert(word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y)==170);
        assert(read_pad(0)==0);
    }else assert(!"unknown case");
    puts("PASS actual cellPad HLE positive-report hold, guest serialization and preserved input semantics");
}

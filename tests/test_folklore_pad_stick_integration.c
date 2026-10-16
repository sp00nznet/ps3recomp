/* Optional mapping through actual cellPad capability, enable and BE report APIs. */
#ifndef _WIN32
#include <time.h>
static unsigned long long test_ms=100;
static int test_clock_gettime(clockid_t id,struct timespec* ts)
{ (void)id;ts->tv_sec=test_ms/1000;ts->tv_nsec=(test_ms%1000)*1000000;return 0; }
#define clock_gettime test_clock_gettime
#endif
#include "pad_sdl_fixture.h"

static void advance(void)
{
#ifdef _WIN32
    fake_ms+=20;
#else
    test_ms+=20;
#endif
}
static void sample(void)
{
    advance();pad_poll_backend();
    CHECK(cellPadGetData(0,(CellPadData*)(uintptr_t)0x10000)==0);
}
static void mapping(const char* value)
{
#ifdef _WIN32
    _putenv_s("PS3RECOMP_CAPTURE_MAPPING",value);
#else
    setenv("PS3RECOMP_CAPTURE_MAPPING",value,1);
#endif
}
static const char* pad_path;
static void file_input(unsigned mask,unsigned ry)
{
    FILE* f=fopen(pad_path,"wb");CHECK(f!=NULL);
    if(f){fprintf(f,"%u 1 128 128 128 %u\n",mask,ry);fclose(f);}
}
int main(int argc,char** argv)
{
    if(argc!=2)return 2;
    pad_path=argv[1];file_input(0,128);
#ifdef _WIN32
    _putenv_s("PAD_FILE",pad_path);
#else
    setenv("PAD_FILE",pad_path,1);
#endif
    static uint8_t memory[0x20000];vm_base=memory;
    fixture.capable=0;
    mapping("off");CHECK(cellPadInit(1)==0);sample();
    CHECK(cellPadInfoSensorMode(0)==0);
    CHECK((u32)cellPadSetSensorMode(0,1)==CELL_PAD_ERROR_UNSUPPORTED_GAMEPAD);
    CHECK(length()==8);CHECK(cellPadEnd()==0);

    mapping("fallback");advance();CHECK(cellPadInit(1)==0);sample();
    CHECK(cellPadInfoSensorMode(0)==1);
    CHECK(cellPadGetCapabilityInfo(0,(CellPadCapabilityInfo*)(uintptr_t)0x13000)==0);
    CHECK(vm_read32(0x13000)&CELL_PAD_CAPABILITY_SENSOR_MODE);
    CHECK(cellPadSetSensorMode(0,1)==0);sample();CHECK(length()==24&&word(21)==399);
    fixture_axes[SDL_CONTROLLER_AXIS_RIGHTY]=-32768;sample();CHECK(word(21)==399);
    fixture_buttons=1u<<SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;sample();CHECK(word(21)==399);
    fixture_axes[SDL_CONTROLLER_AXIS_RIGHTY]=0;sample();CHECK(word(21)==399);
    fixture_axes[SDL_CONTROLLER_AXIS_RIGHTY]=-32768;sample();
    CHECK(length()==24&&word(20)==512&&word(21)==60&&word(22)==512&&word(23)==512);
    CHECK(memory[0x10004+21*2]==0&&memory[0x10005+21*2]==60);
    for(int i=0;i<4;i++)sample();CHECK(word(21)==738);
    for(int i=0;i<4;i++)sample();CHECK(word(21)==399);
    for(int i=0;i<20;i++){sample();CHECK(word(21)==399);}
    fixture_buttons=0;fixture_axes[SDL_CONTROLLER_AXIS_RIGHTY]=0;sample();
    file_input(CELL_PAD_CTRL_R1,128);sample();CHECK(word(21)==399);
    file_input(CELL_PAD_CTRL_R1,0);sample();CHECK(word(21)==60);
    sample();CHECK(word(21)==399); /* expired file returns to live released R1 */
    fixture_buttons=1u<<SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    fixture_axes[SDL_CONTROLLER_AXIS_RIGHTY]=0;sample();
    fixture_axes[SDL_CONTROLLER_AXIS_RIGHTY]=-32768;sample();CHECK(word(21)==60);
    CHECK(cellPadSetSensorMode(0,0)==0);sample();CHECK(length()==8&&word(21)==0);
    CHECK(cellPadSetPortSetting(0,CELL_PAD_SETTING_SENSOR_ON)==0);sample();CHECK(word(21)==399);

    /* Switching to real sensors never substitutes synthetic data on read failure. */
    fixture.capable=1;sample();CHECK(!s_stick_mapped[0]&&word(23)==635);
    fixture.read_fail=1;sample();CHECK(length()==8&&word(21)==0);
    fixture.read_fail=0;sample();CHECK(word(23)==635);
    fixture.attached=0;sample();CHECK(!s_stick_motion[0].firing);
    fixture.attached=1;fixture_instance++;fixture.capable=0;sample();CHECK(word(21)==399);
    CHECK(cellPadEnd()==0);

    mapping("force");fixture.capable=1;advance();CHECK(cellPadInit(1)==0);sample();
    CHECK(cellPadSetSensorMode(0,1)==0);sample();CHECK(s_stick_mapped[0]&&word(23)==512);
    CHECK(cellPadEnd()==0);

#ifndef _WIN32
    /* Same existing focus-aware keyboard bridge supplies W + Up. */
    mapping("fallback");fixture.attached=0;advance();CHECK(cellPadInit(1)==0);
    ps3_pad_keyboard_attach(7);ps3_pad_keyboard_focus(7,1);sample();
    CHECK(cellPadSetSensorMode(0,1)==0);
    ps3_pad_keyboard_key(7,CELL_PAD_CTRL_R1,1,0);sample();CHECK(word(21)==399);
    ps3_pad_keyboard_key(7,CELL_PAD_CTRL_UP,1,0);sample();CHECK(word(21)==60);
    ps3_pad_keyboard_focus(7,0);sample();CHECK(word(21)==399&&!s_stick_motion[0].firing);
    CHECK(cellPadEnd()==0);
#endif
    mapping("invalid");fixture.attached=1;fixture.capable=0;advance();CHECK(cellPadInit(1)==0);sample();
    CHECK(cellPadInfoSensorMode(0)==0);CHECK(cellPadEnd()==0);
    printf("stick motion guest integration: %d checks, %d failures\n",checks,failures);
    remove(pad_path);
    return failures?1:0;
}

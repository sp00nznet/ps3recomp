/* CPU-only guest ABI test: every controller and both raw backends are mocked. */
#define PAD_FIXTURE_G7_MOCK 1
#define PAD_FIXTURE_DS3_MOCK 1
#include "pad_sdl_fixture.h"

static void choose_ds3(int sensors,ds3_motion_state initial,int start_ok)
{
    ++fixture_instance;fixture_vid=0x054c;fixture_pid=0x0268;
    fixture.capable=sensors;fixture.attached=1;
    fixture_ds3_hold_stop=0;fixture_ds3_start_ok=start_ok;
    fixture_ds3_start_state=initial;
    read_pad();
}
static void check_capability(int expected)
{
    CHECK(s_host_state[0].motion_capable==expected);
    CHECK(cellPadInfoSensorMode(0)==expected);
    CHECK(cellPadGetInfo2((CellPadInfo2*)(uintptr_t)0x11000)==0);
    CHECK(!!(vm_read32(0x11000+offsetof(CellPadInfo2,device_capability))&
             CELL_PAD_CAPABILITY_SENSOR_MODE)==expected);
    CHECK(cellPadPeriphGetInfo((CellPadPeriphInfo*)(uintptr_t)0x12000)==0);
    CHECK(!!(vm_read32(0x12000+offsetof(CellPadPeriphInfo,device_capability))&
             CELL_PAD_CAPABILITY_SENSOR_MODE)==expected);
    CHECK(cellPadGetCapabilityInfo(0,(CellPadCapabilityInfo*)(uintptr_t)0x13000)==0);
    CHECK(!!(vm_read32(0x13000)&CELL_PAD_CAPABILITY_SENSOR_MODE)==expected);
}
static void make_fresh_sample(void)
{
    fixture_ds3_raw.state=DS3_MOTION_RUNNING;fixture_ds3_raw.fresh_valid=1;
    fixture_ds3_raw.sensor[0]=291;fixture_ds3_raw.sensor[1]=683;
    fixture_ds3_raw.sensor[2]=85;fixture_ds3_raw.sensor[3]=1023;
}
static void test_native_guest_words(void)
{
    int before=fixture_ds3_starts;
    choose_ds3(0,DS3_MOTION_RUNNING,1);
    CHECK(fixture_ds3_starts==before+1);
    CHECK(fixture_ds3_started_vid==0x054c&&fixture_ds3_started_pid==0x0268);
    check_capability(1);
    CHECK(cellPadSetSensorMode(0,0)==0);make_fresh_sample();read_pad();
    CHECK(length()==8&&word(20)==0&&word(23)==0);
    CHECK(cellPadSetSensorMode(0,1)==0);read_pad();
    CHECK(length()==24&&word(1)==0x7c);
    CHECK(word(20)==291&&word(21)==683&&word(22)==85&&word(23)==1023);
    const unsigned char expected[]={1,0x23,2,0xab,0,0x55,3,0xff};
    CHECK(memcmp(vm_base+0x10004+20*2,expected,sizeof(expected))==0);
    s_stick_mode=1;pad_poll_stick_motion(0);
    CHECK(!s_stick_mapped[0]&&s_host_state[0].motion_capable&&s_host_state[0].motion_valid);
    CHECK(s_host_state[0].motion[0]==291&&s_host_state[0].motion[3]==1023);
    s_stick_mode=2;pad_poll_stick_motion(0);
    CHECK(s_stick_mapped[0]&&s_host_state[0].motion_capable&&!s_host_state[0].motion_valid);
    s_stick_mode=0;pad_poll_stick_motion(0);read_pad();
    CHECK(!s_stick_mapped[0]&&word(20)==291&&word(23)==1023);
    CHECK(cellPadSetSensorMode(0,0)==0);read_pad();
    CHECK(length()==8&&word(20)==0&&word(23)==0);
    CHECK(cellPadSetPortSetting(0,CELL_PAD_SETTING_SENSOR_ON|CELL_PAD_SETTING_PRESS_ON)==0);
    read_pad();CHECK(length()==24&&word(23)==1023);
    fixture_ds3_raw.fresh_valid=0;read_pad();
    CHECK(length()==20&&word(20)==0&&word(21)==0&&word(22)==0&&word(23)==0);
    CHECK(!s_host_state[0].motion_valid);
    check_capability(1); /* stale counts are suppressed; working device remains capable */
}
static void test_sensor_precedence_and_other_controllers(void)
{
    int before=fixture_ds3_starts;
    choose_ds3(1,DS3_MOTION_RUNNING,1);
    CHECK(fixture_ds3_starts==before&&s_ds3_port==-1);
    check_capability(1);CHECK(cellPadSetSensorMode(0,1)==0);
    read_pad();CHECK(length()==24&&word(21)==399&&word(23)==635);
    ++fixture_instance;fixture_vid=0x1234;fixture_pid=0x5678;read_pad();
    CHECK(fixture_ds3_starts==before&&s_ds3_port==-1);
    check_capability(1);read_pad();CHECK(length()==24&&word(23)==635);
    fixture.capable=0;++fixture_instance;read_pad();
    CHECK(fixture_ds3_starts==before);check_capability(0);
}
static void test_failures_and_retry(void)
{
    int before=fixture_ds3_starts;
    choose_ds3(0,DS3_MOTION_RUNNING,0);
    CHECK(fixture_ds3_starts==before+1);check_capability(0);
    fixture_ticks+=10000;read_pad();CHECK(fixture_ds3_starts==before+1);
    const ds3_motion_state states[]={DS3_MOTION_IO_ERROR,DS3_MOTION_UNSUPPORTED,DS3_MOTION_AMBIGUOUS};
    for(size_t i=0;i<sizeof(states)/sizeof(states[0]);++i) {
        before=fixture_ds3_starts;
        choose_ds3(0,states[i],1);fixture_ds3_idle=1;check_capability(0);
        for(int n=0;n<3;++n) {
            fixture_ticks+=1000;read_pad();CHECK(fixture_ds3_starts==before+1);
        }
    }
    before=fixture_ds3_starts;
    choose_ds3(0,DS3_MOTION_NO_DEVICE,1);check_capability(0);
    CHECK(fixture_ds3_starts==before+1);
    Uint64 started=fixture_ticks;fixture_ds3_idle=1;
    for(int n=0;n<10;++n)read_pad();CHECK(fixture_ds3_starts==before+1);
    fixture_ticks=started+999;read_pad();CHECK(fixture_ds3_starts==before+1);
    fixture_ticks=started+1000;read_pad();CHECK(fixture_ds3_starts==before+2);
    fixture_ds3_idle=1;
    fixture_ticks=started+1999;read_pad();CHECK(fixture_ds3_starts==before+2);
    fixture_ticks=started+2000;read_pad();CHECK(fixture_ds3_starts==before+3);
    fixture_ds3_idle=1;
    for(int n=0;n<4;++n) {
        fixture_ticks+=1000;read_pad();CHECK(fixture_ds3_starts==before+3);
    }
    check_capability(0);
    before=fixture_ds3_starts;choose_ds3(0,DS3_MOTION_STARTING,1);
    CHECK(fixture_ds3_starts==before+1);check_capability(1);
    CHECK(!s_host_state[0].motion_valid);
}
static void test_disconnect_ambiguity_and_completion(void)
{
    choose_ds3(0,DS3_MOTION_RUNNING,1);make_fresh_sample();read_pad();
    int before=fixture_ds3_starts,stops=fixture_ds3_stops;
    fixture.attached=0;read_pad();
    CHECK(fixture_ds3_stops==stops+1&&fixture_ds3_starts==before);
    CHECK(!s_host_state[0].connected&&!s_host_state[0].motion_valid);
    CHECK(word(20)==0&&word(23)==0);
    choose_ds3(0,DS3_MOTION_RUNNING,1);CHECK(fixture_ds3_starts==before+1);
    before=fixture_ds3_starts;
    fixture_second_vid=0x054c;fixture_second_pid=0x0268;
    fixture_second.attached=1;read_pad();
    CHECK(s_ds3_port==-1&&fixture_ds3_starts==before);
    CHECK(!s_host_state[0].motion_capable&&!s_host_state[1].motion_capable);
    fixture_second.attached=0;read_pad();
    CHECK(fixture_ds3_starts==before+1);check_capability(1);
    before=fixture_ds3_starts;stops=fixture_ds3_stops;
    fixture_ds3_hold_stop=1;++fixture_instance;read_pad();
    CHECK(fixture_ds3_stops==stops+1&&fixture_ds3_starts==before);
    check_capability(0);
    for(int n=0;n<3;++n) {fixture_ticks+=1000;read_pad();CHECK(fixture_ds3_starts==before);}
    fixture_ds3_idle=1;fixture_ds3_hold_stop=0;read_pad();
    CHECK(fixture_ds3_starts==before+1);check_capability(1);
    CHECK(fixture_ds3_services>0);
}
int main(void)
{
    static uint8_t memory[0x20000];vm_base=memory;
    CHECK(cellPadInit(2)==0);
    test_native_guest_words();test_sensor_precedence_and_other_controllers();
    test_failures_and_retry();test_disconnect_ambiguity_and_completion();
    CHECK(fixture_g7_starts==0);CHECK(cellPadEnd()==0);
    printf("DS3 cellPad integration: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}

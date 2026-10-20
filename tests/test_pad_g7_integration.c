/* All controller and raw G7 APIs are mocked. No HID/device access or sleeps. */
#define PAD_FIXTURE_G7_MOCK 1
#include "pad_sdl_fixture.h"

static void choose_device(Uint16 vid,Uint16 pid,int sensors,
                          g7_motion_state initial,int start_ok)
{
    ++fixture_instance;
    fixture_vid=vid;fixture_pid=pid;fixture.capable=sensors;
    fixture_g7_start_state=initial;fixture_g7_start_ok=start_ok;
    fixture_g7_hold_stop=0;
    read_pad();
}
static void check_capability(int expected)
{
    CHECK(s_host_state[0].motion_capable==expected);
    CHECK(cellPadInfoSensorMode(0)==expected);
    CHECK(cellPadGetInfo2((CellPadInfo2*)(uintptr_t)0x11000)==0);
    CHECK(!!(vm_read32(0x11000+offsetof(CellPadInfo2,device_capability))&
             CELL_PAD_CAPABILITY_SENSOR_MODE)==expected);
    CHECK(cellPadGetCapabilityInfo(0,(CellPadCapabilityInfo*)(uintptr_t)0x13000)==0);
    CHECK(!!(vm_read32(0x13000)&CELL_PAD_CAPABILITY_SENSOR_MODE)==expected);
}
static void test_ids_and_sensor_precedence(void)
{
    for(int i=0;i<2;++i) {
        Uint16 pid=i?0x10c6:0x10c5;
        int before=fixture_g7_starts;
        choose_device(0x3537,pid,0,G7_MOTION_RUNNING,1);
        CHECK(fixture_g7_starts==before+1);
        CHECK(fixture_g7_started_vid==0x3537&&fixture_g7_started_pid==pid);
        check_capability(1);
        CHECK(cellPadSetSensorMode(0,1)==0);
        fixture_g7_raw.fresh_valid=1;
        fixture_g7_raw.accel[0]=0;
        fixture_g7_raw.accel[1]=8182;
        fixture_g7_raw.accel[2]=0;
        read_pad();
        CHECK(length()==24&&word(20)==512&&word(21)==399&&word(23)==512);
        fixture_g7_raw.fresh_valid=0;
        read_pad();
        CHECK(length()==8&&word(20)==0&&word(23)==0);
        check_capability(1); /* stale sample does not remove working hardware */
    }
    int before=fixture_g7_starts;
    choose_device(0x3537,0x10c5,1,G7_MOTION_RUNNING,1);
    CHECK(fixture_g7_starts==before&&s_g7_port==-1);
    check_capability(1);
    CHECK(cellPadSetSensorMode(0,1)==0);
    read_pad();CHECK(length()==24&&word(23)==635);
    before=fixture_g7_starts;
    choose_device(0x3537,0x9999,0,G7_MOTION_RUNNING,1);
    CHECK(fixture_g7_starts==before&&s_g7_port==-1);
    check_capability(0);
    choose_device(0x9999,0x10c5,0,G7_MOTION_RUNNING,1);
    CHECK(fixture_g7_starts==before&&s_g7_port==-1);
    check_capability(0);
}
static void test_failed_start_and_terminal_states(void)
{
    int before=fixture_g7_starts;
    choose_device(0x3537,0x10c5,0,G7_MOTION_RUNNING,0);
    CHECK(fixture_g7_starts==before+1);
    check_capability(0);
    fixture_ticks+=10000;read_pad();
    CHECK(fixture_g7_starts==before+1);
    const g7_motion_state terminal[]={G7_MOTION_UNSUPPORTED,G7_MOTION_IO_ERROR};
    for(int i=0;i<2;++i) {
        before=fixture_g7_starts;
        choose_device(0x3537,0x10c6,0,terminal[i],1);
        CHECK(fixture_g7_starts==before+1);
        fixture_g7_idle=1; /* completed terminal worker */
        check_capability(0);
        for(int n=0;n<4;++n) {
            fixture_ticks+=1000;read_pad();
            CHECK(fixture_g7_starts==before+1);
            check_capability(0);
        }
    }
    before=fixture_g7_starts;
    choose_device(0x3537,0x10c6,0,G7_MOTION_RUNNING,1);
    CHECK(fixture_g7_starts==before+1); /* reconnect permits explicit new attempt */
    check_capability(1);
}
static void test_no_device_retry_budget(void)
{
    int before=fixture_g7_starts;
    choose_device(0x3537,0x10c5,0,G7_MOTION_NO_DEVICE,1);
    CHECK(fixture_g7_starts==before+1);
    check_capability(0);
    Uint64 started=fixture_ticks;
    fixture_g7_idle=1;
    for(int n=0;n<12;++n)read_pad();
    CHECK(fixture_g7_starts==before+1);
    fixture_ticks=started+999;read_pad();
    CHECK(fixture_g7_starts==before+1);
    fixture_ticks=started+1000;read_pad();
    CHECK(fixture_g7_starts==before+2);
    check_capability(0);
    fixture_g7_idle=1;
    fixture_ticks=started+1999;read_pad();
    CHECK(fixture_g7_starts==before+2);
    fixture_ticks=started+2000;read_pad();
    CHECK(fixture_g7_starts==before+3);
    fixture_g7_idle=1;
    for(int n=0;n<5;++n) {
        fixture_ticks+=1000;read_pad();
        CHECK(fixture_g7_starts==before+3);
        check_capability(0);
    }
}
static void test_instance_replacement_waits_for_service(void)
{
    choose_device(0x3537,0x10c5,0,G7_MOTION_RUNNING,1);
    int before=fixture_g7_starts,stops=fixture_g7_stops;
    ++fixture_instance;read_pad();
    CHECK(fixture_g7_stops==stops+1&&fixture_g7_starts==before+1);
    CHECK(fixture_g7_started_pid==0x10c5);
    before=fixture_g7_starts;stops=fixture_g7_stops;
    fixture_g7_hold_stop=1;
    ++fixture_instance;read_pad();
    CHECK(fixture_g7_stops==stops+1&&fixture_g7_starts==before);
    check_capability(0);
    for(int n=0;n<4;++n) {
        fixture_ticks+=1000;read_pad();
        CHECK(fixture_g7_starts==before);
    }
    fixture_g7_idle=1;fixture_g7_hold_stop=0;
    read_pad();CHECK(fixture_g7_starts==before+1);
    check_capability(1);
    CHECK(fixture_g7_services>0);
}
int main(void)
{
    static uint8_t memory[0x20000];vm_base=memory;
    CHECK(cellPadInit(1)==0);
    test_ids_and_sensor_precedence();
    test_failed_start_and_terminal_states();
    test_no_device_retry_budget();
    test_instance_replacement_waits_for_service();
    CHECK(cellPadEnd()==0);
    printf("G7 cellPad integration: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}

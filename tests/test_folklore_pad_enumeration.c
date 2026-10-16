#include "pad_sdl_fixture.h"
int main(void)
{
    static uint8_t memory[0x20000];vm_base=memory;
    leading_unmapped=3;
    CHECK(cellPadInit(1)==0);
    read_pad();
    CHECK(s_host_state[0].connected && s_sdl_controllers[0]==controller());
    CHECK(fixture_opens==1 && fixture_closes==0);
    CHECK(length()==8 && word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X)==128);
    CHECK(cellPadSetSensorMode(0,1)==0);
    read_pad();CHECK(length()==24 && word(21)==399);
    leading_unmapped=8; /* Mapped controller beyond guest port-count limit. */
    read_pad();CHECK(s_host_state[0].connected && fixture_opens==1);
    CHECK(length()==24 && word(21)==399);
    leading_unmapped=0;read_pad();CHECK(fixture_opens==1 && fixture_closes==0);
    fixture.attached=0;read_pad();
    CHECK(!s_host_state[0].connected && fixture_closes==1);
    fixture.attached=1;fixture_instance=43;leading_unmapped=2;
    fixture_open_fail=1;read_pad();CHECK(!s_host_state[0].connected && fixture_opens==1);
    fixture_open_fail=0;read_pad();
    CHECK(s_host_state[0].connected && fixture_opens==2 && s_sdl_instance[0]==43);
    for(int i=1;i<PAD_MAX_HOST_PORTS;++i)CHECK(!s_sdl_controllers[i]);
    CHECK(cellPadEnd()==0);CHECK(fixture_closes==2);
    printf("pad enumeration: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}

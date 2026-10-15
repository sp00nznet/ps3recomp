/* Synthetic command semantics and FIFO advancement; no GPU or game data. */
#include "../libs/video/rsx_commands.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint32_t ppu_hle_inject_base;
static unsigned writes,marks,clears,targets;
void vm_write32(uint32_t address,uint32_t value) {(void)address;(void)value;++writes;}
void ps3_ms(const char* key) {(void)key;++marks;}
static void clear(void* user,u32 mask,u32 color,float depth,u8 stencil)
{(void)user;(void)mask;(void)color;(void)depth;(void)stencil;++clears;}
static void target(void* user,const rsx_state* state) {(void)user;(void)state;++targets;}
int main(int argc,char** argv)
{
    rsx_state state;rsx_state_init(&state);
    rsx_backend backend={0};backend.clear=clear;backend.set_render_target=target;
    rsx_set_backend(&backend);
    if(argc==2) {
        assert(!strcmp(argv[1],"notify"));
        rsx_process_method(&state,0x0104,0);return 99;
    }
    rsx_state before=state;
    const u32 values[]={0,1,0xdeadbeef,0xffffffff};
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i) {
        assert(rsx_process_method(&state,NV4097_NO_OPERATION,values[i])==0);
        assert(!memcmp(&state,&before,sizeof(state)));
    }
    const u32 fifo[]={
        (1u<<18)|NV4097_NO_OPERATION,0xdeadbeef,
        (1u<<18)|NV4097_SET_COLOR_CLEAR_VALUE,0x12345678,
        (2u<<29)|(3u<<18)|NV4097_NO_OPERATION,0,1,0xffffffff,
        (1u<<18)|NV4097_SET_DEPTH_FUNC,0x0204
    };
    assert(rsx_process_command_buffer(&state,fifo,sizeof(fifo))==6);
    before.color_clear_value=0x12345678;before.depth_func=0x0204;before.depth_dirty=1;
    assert(!memcmp(&state,&before,sizeof(state)));
    assert(!writes && !marks && !clears && !targets);
    puts("NOP payloads, immutable state, FIFO advancement and no callbacks verified");
    return 0;
}

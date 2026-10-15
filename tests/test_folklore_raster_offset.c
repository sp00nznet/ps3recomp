/* Synthetic attachment-address tests. No game payload or graphics device. */
#include "../libs/video/rsx_commands.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint32_t ppu_hle_inject_base;
void vm_write32(uint32_t address, uint32_t value) { (void)address; (void)value; }
void ps3_ms(const char* key) { (void)key; }
static u32 observed[4];
static void target(void* user, const rsx_state* state)
{
    (void)user;
    for (u32 i=0; i<4; ++i) assert(rsx_color_surface_offset(state,i,&observed[i])==0);
}
int main(void)
{
    rsx_state state;
    rsx_state_init(&state);
    rsx_backend backend={0};
    backend.set_render_target=target;
    rsx_set_backend(&backend);
    state.surface_dirty=0;
    assert(rsx_process_method(&state,NV4097_SET_WINDOW_OFFSET,0x0007000du)==0);
    assert(state.window_offset==0x0007000du && state.surface_dirty);
    /* Literal addresses for x13/y7, row pitch5120, base1MiB. */
    const struct { u32 format, expected; } vectors[]={
        {1,0x108c1a},{2,0x108c1a},{3,0x108c1a},{4,0x108c34},
        {5,0x108c34},{8,0x108c34},{9,0x108c0d},{10,0x108c1a},
        {11,0x108c68},{12,0x108cd0},{13,0x108c34},
        {14,0x108c34},{15,0x108c34},{16,0x108c34}};
    for (u32 i=0; i<4; ++i) {
        state.surface_color_offset[i]=0x100000;
        state.surface_color_pitch[i]=5120;
        for (unsigned j=0; j<sizeof(vectors)/sizeof(vectors[0]); ++j) {
            state.surface_format=vectors[j].format;
            u32 value=0;
            assert(rsx_color_surface_offset(&state,i,&value)==0);
            assert(value==vectors[j].expected);
            assert(state.surface_color_offset[i]==0x100000);
        }
    }
    state.surface_zeta_offset=0x100000; state.surface_zeta_pitch=5120;
    u32 value=0;
    state.surface_format=1u<<5;
    assert(rsx_depth_surface_offset(&state,&value)==0 && value==0x108c1a);
    state.surface_format=2u<<5;
    assert(rsx_depth_surface_offset(&state,&value)==0 && value==0x108c34);
    /* Different attachments retain independent pitches/bases. */
    state.surface_format=8;
    state.surface_color_pitch[1]=4096; state.surface_color_offset[1]=0x200000;
    assert(rsx_process_method(&state,NV4097_SET_BEGIN_END,5)==0);
    assert(observed[0]==0x108c34 && observed[1]==0x207034);
    assert(!state.surface_dirty);
    state.surface_dirty=0;
    assert(rsx_process_method(&state,NV4097_SET_SURFACE_PITCH_A,1024)==0 && state.surface_dirty);
    assert(rsx_color_surface_offset(&state,0,&value)==0 && value==0x101c34);
    assert(rsx_color_surface_offset(&state,0,&value)==0 && value==0x101c34); /* no accumulation */
    /* Failed queries never publish a wrapped or guessed address. */
    value=0x12345678;
    state.surface_color_offset[0]=0xfffffff0u;
    assert(rsx_color_surface_offset(&state,0,&value)==-1 && value==0x12345678);
    state.surface_color_offset[0]=0x100000;
    state.surface_color_pitch[0]=0;
    assert(rsx_color_surface_offset(&state,0,&value)==-1 && value==0x12345678);
    state.surface_color_pitch[0]=1024; state.surface_format=7;
    assert(rsx_color_surface_offset(&state,0,&value)==-1 && value==0x12345678);
    assert(rsx_depth_surface_offset(&state,&value)==-1 && value==0x12345678);
    assert(rsx_color_surface_offset(NULL,0,&value)==-1);
    assert(rsx_color_surface_offset(&state,4,&value)==-1);
    assert(rsx_color_surface_offset(&state,0,NULL)==-1);
    assert(rsx_depth_surface_offset(NULL,&value)==-1);
    assert(rsx_depth_surface_offset(&state,NULL)==-1);
    /* Zero offset is valid before formats/pitches are initialized. */
    assert(rsx_process_method(&state,NV4097_SET_WINDOW_OFFSET,0)==0);
    assert(rsx_color_surface_offset(&state,0,&value)==0 && value==0x100000);
    /* High16 Y and low16 X, with64-bit intermediate arithmetic. */
    state.surface_format=9; state.surface_color_offset[0]=0;
    state.surface_color_pitch[0]=65536; state.window_offset=0xffffffffu;
    assert(rsx_color_surface_offset(&state,0,&value)==0 && value==UINT32_MAX);
    state.surface_color_offset[0]=1;
    assert(rsx_color_surface_offset(&state,0,&value)==-1 && value==UINT32_MAX);
    puts("raster attachment vectors, binding, dirty state and failure invariants passed");
    return 0;
}

/* Synthetic immediate streams through the actual Windows GPU renderer. */
#include <windows.h>
#include "live_draw_test_harness.h"
#define T "test_live_draw_immediate"
static void color(float r,float g,float b,float a) {
    const float values[4]={r,g,b,a};
    for(u32 i=0;i<4;i++)ldh_mf(0x1C30+i*4,values[i]);
}
static void position(float x,float y,int four) {
    const float values[4]={x,y,0,1};u32 method=four?0x1C00:0x1500;
    for(u32 i=0;i<(four?4u:3u);i++)ldh_mf(method+i*4,values[i]);
}
static void clear_blue(void) {
    ldh_m(NV4097_SET_COLOR_CLEAR_VALUE,0xFF0000FF);
    ldh_m(NV4097_CLEAR_SURFACE,0xF0);
}
static void quad(int four) {
    position(-.8f,-.8f,four);position(.8f,-.8f,four);
    position(-.8f,.8f,four);position(.8f,.8f,four);
}
static int center(unsigned channel) {
    uint8_t rgb[8*8*3];u32 w=0,h=0;
    if(ldh_read_rgb(0x10000,"immediate.ppm",rgb,8,8,&w,&h)||w!=8||h!=8)return -1;
    return rgb[(4*8+4)*3+channel];
}
int main(void) {
    if(ldh_init(T))return 2;
    ShowWindow((HWND)rsx_null_backend_get_hwnd(),SW_HIDE);
    rsx_test_fp_mov_r0_col0(ldh_main+0x1000,RSX_TEST_FP_SWZ_IDENT);
    ldh_setup_quad(0,1,1,0,0);
    ldh_surface(0x8u|(1u<<8),0x10000,32,0x200000,32,8,8);
    ldh_m(NV4097_SET_DEPTH_TEST_ENABLE,0);ldh_m(NV4097_SET_DEPTH_MASK,0);
    ldh_m(NV4097_SET_SHADER_CONTROL,0x40);ldh_m(NV4097_SET_SHADER_PROGRAM,0x1002);
    /* Array descriptors do not describe an immediate stream. */
    ldh_m(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT,0);
    ldh_m(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET,0x7FFFFFF0);
    clear_blue();color(1,0,0,1);ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_TRIANGLE_STRIP);
    quad(0);ldh_m(NV4097_SET_BEGIN_END,0);
    ldh_check("DATA3F positions draw without vertex arrays",center(0)>200 && center(2)<10);
    clear_blue();color(0,1,0,1);ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_TRIANGLE_STRIP);
    quad(1);ldh_m(NV4097_SET_BEGIN_END,0);
    ldh_check("DATA4F positions and zero color channels retained",center(1)>200 && center(0)<10);
    clear_blue();ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_TRIANGLES);
    color(1,0,0,1);position(-.8f,-.8f,0);
    color(0,1,0,1);position(.8f,-.8f,0);
    color(0,0,1,1);position(0,.8f,0);
    ldh_m(NV4097_SET_BEGIN_END,0);
    ldh_check("attributes snapshot at each position completion",center(0)>20 && center(1)>20 && center(2)>20);
    rsx_test_fp_mov_r0_col0(ldh_main+0x1100,RSX_TEST_FP_SWZ(3,3,3,3));
    ldh_m(NV4097_SET_SHADER_PROGRAM,0x1102);
    clear_blue();color(0,0,0,0);ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_TRIANGLE_STRIP);
    quad(0);ldh_m(NV4097_SET_BEGIN_END,0);
    ldh_check("explicit all-zero current attribute retains zero alpha",center(0)<10 && center(1)<10 && center(2)<10);
    ldh_m(NV4097_SET_SHADER_PROGRAM,0x1002);
    clear_blue();color(1,0,0,1);ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_TRIANGLE_STRIP);
    quad(0);ldh_m(NV4097_DRAW_ARRAYS,3u<<24);ldh_m(NV4097_SET_BEGIN_END,0);
    ldh_check("mixed immediate/array group rejected entirely",center(2)>200 && center(0)<10);
    clear_blue();ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_TRIANGLE_STRIP);
    quad(0);ldh_m(0x1818,0);ldh_m(NV4097_SET_BEGIN_END,0);
    ldh_check("mixed immediate/invalid-inline group rejected entirely",center(2)>200 && center(0)<10);
    clear_blue();ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_TRIANGLE_STRIP);
    for(unsigned i=0;i<4097;i++)position((i&1)?.8f:-.8f,(i&2)?.8f:-.8f,0);
    ldh_m(NV4097_SET_BEGIN_END,0);
    ldh_check("overlong group does not render a partial prefix",center(2)>200 && center(0)<10);
    clear_blue();ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_TRIANGLES);
    ldh_mf(0x1500,-.8f);ldh_mf(0x1504,-.8f);ldh_m(NV4097_SET_BEGIN_END,0);
    ldh_check("incomplete position emits no vertex",center(2)>200 && center(0)<10);
    clear_blue();ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_TRIANGLE_STRIP);
    position(-.8f,-.8f,0);position(.8f,-.8f,0);ldh_m(NV4097_SET_BEGIN_END,0);
    ldh_check("incomplete primitive emits no partial triangle",center(2)>200 && center(0)<10);
    color(1,0,0,1);quad(0);
    ldh_check("positions outside BEGIN do not draw",center(2)>200 && center(0)<10);
    rsx_live_draw_shutdown();return ldh_done(T);
}

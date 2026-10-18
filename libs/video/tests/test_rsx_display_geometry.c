#include "../rsx_display_geometry.h"
#include <stdio.h>
#include <string.h>
static unsigned checks,failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while(0)
static void fit(uint32_t sw,uint32_t sh,uint32_t dw,uint32_t dh,
                uint32_t x,uint32_t y,uint32_t w,uint32_t h)
{
    rsx_display_rect r;
    CHECK(rsx_display_fit(sw,sh,dw,dh,&r)==0);
    CHECK(r.x==x && r.y==y && r.w==w && r.h==h);
    CHECK((uint64_t)r.x+r.w<=dw && (uint64_t)r.y+r.h<=dh);
}
int main(void)
{
    fit(1280,720,1920,1080,0,0,1920,1080);
    fit(1280,720,1280,800,0,40,1280,720);
    fit(1280,720,3440,1440,440,0,2560,1440);
    fit(1280,720,5120,1440,1280,0,2560,1440);
    fit(1280,720,1080,1920,0,656,1080,607);
    fit(1280,720,1,1,0,0,1,1);
    fit(1,UINT32_MAX,UINT32_MAX,1,2147483647,0,1,1);
    fit(UINT32_MAX,1,1,UINT32_MAX,0,2147483647,1,1);
    fit(UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,0,0,UINT32_MAX,UINT32_MAX);
    /* Resize a letterboxed canvas back to 16:9: crop its known content first. */
    fit(2560,1440,1920,1080,0,0,1920,1080);
    rsx_display_rect r={7,8,9,10},saved=r;
    CHECK(rsx_display_fit(0,1,1,1,&r)==-1);
    CHECK(rsx_display_fit(1,0,1,1,&r)==-1);
    CHECK(rsx_display_fit(1,1,0,1,&r)==-1);
    CHECK(rsx_display_fit(1,1,1,0,&r)==-1);
    CHECK(rsx_display_fit(1,1,1,1,0)==-1);
    CHECK(memcmp(&r,&saved,sizeof r)==0);
    CHECK(rsx_display_extent_valid(7680,4320,8192));
    CHECK(rsx_display_extent_valid(5120,1440,8192));
    CHECK(rsx_display_extent_valid(8192,4096,8192));
    CHECK(!rsx_display_extent_valid(8192,4097,8192));
    CHECK(!rsx_display_extent_valid(0,720,8192));
    CHECK(!rsx_display_extent_valid(8193,1,16384));
    CHECK(!rsx_display_extent_valid(4096,2160,2048));
    CHECK(!rsx_display_extent_valid(UINT32_MAX,UINT32_MAX,UINT32_MAX));
    uint32_t ww=0,wh=0;
    CHECK(rsx_display_window_size(3440,1440,2500,1300,&ww,&wh)==0);
    CHECK(ww==2500 && wh==1046);
    CHECK(rsx_display_window_size(1280,720,2500,1300,&ww,&wh)==0);
    CHECK(ww==1280 && wh==720);
    CHECK(rsx_display_window_size(3440,1440,2500,0,&ww,&wh)==-1);
    int display=7;
    CHECK(rsx_display_parse_index("1",2,&display)==0 && display==1);
    CHECK(rsx_display_parse_index("0",2,&display)==0 && display==0);
    CHECK(rsx_display_parse_index("2",2,&display)==-1 && display==0);
    CHECK(rsx_display_parse_index("-1",2,&display)==-1);
    CHECK(rsx_display_parse_index("1x",2,&display)==-1);
    CHECK(rsx_display_parse_index("999999999999999",2,&display)==-1);
    printf("display geometry: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}

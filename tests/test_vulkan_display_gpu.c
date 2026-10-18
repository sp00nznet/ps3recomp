/* Real Vulkan/SDL output canvas, content-fit and window transition checks. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
#ifdef _WIN32
#include <SDL2/SDL_syswm.h>
#endif
uint8_t* vm_base;
static unsigned checks;
#define CHECK(v) do { ++checks; if (!(v)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#v); return 1; } } while(0)
static void env(const char* k,const char* v)
{
#ifdef _WIN32
    _putenv_s(k,v);
#else
    setenv(k,v,1);
#endif
}
static int pump(void)
{
    SDL_Delay(80);
    return rsx_vulkan_backend_pump_messages();
}
static int toggle(void)
{
    SDL_Event e; memset(&e,0,sizeof e);
    e.type=SDL_KEYDOWN;e.key.windowID=SDL_GetWindowID(s_vk.window);
    e.key.keysym.sym=SDLK_RETURN;e.key.keysym.mod=KMOD_ALT;
    if (SDL_PushEvent(&e)!=1) return -1;
    return pump();
}
static int check_monitor_layout(int selected,int fullscreen)
{
    CHECK(s_vk.window && s_vk.selected_display==selected);
    CHECK(s_vk.width==3440 && s_vk.height==1440);
    CHECK(SDL_GetWindowDisplayIndex(s_vk.window)==selected);
    SDL_Rect display,usable;
    CHECK(SDL_GetDisplayBounds(selected,&display)==0);
    CHECK(SDL_GetDisplayUsableBounds(selected,&usable)==0);
    CHECK(display.w>0 && display.h>0 && usable.w>0 && usable.h>0);
#ifdef _WIN32
    SDL_SysWMinfo info;SDL_VERSION(&info.version);
    CHECK(SDL_GetWindowWMInfo(s_vk.window,&info));
    HWND window=info.info.win.window;
    RECT outer,client;
    CHECK(GetWindowRect(window,&outer) && GetClientRect(window,&client));
    POINT client_top={client.left,client.top},client_bottom={client.right,client.bottom};
    CHECK(ClientToScreen(window,&client_top) && ClientToScreen(window,&client_bottom));
    client.left=client_top.x;client.top=client_top.y;
    client.right=client_bottom.x;client.bottom=client_bottom.y;
    HMONITOR monitor=MonitorFromWindow(window,MONITOR_DEFAULTTONULL);
    MONITORINFO native={sizeof native};
    CHECK(monitor && GetMonitorInfoW(monitor,&native));
    CHECK(client.left>=outer.left && client.top>=outer.top &&
          client.right<=outer.right && client.bottom<=outer.bottom);
    if (fullscreen) {
        CHECK(outer.left==native.rcMonitor.left && outer.top==native.rcMonitor.top &&
              outer.right==native.rcMonitor.right && outer.bottom==native.rcMonitor.bottom);
    } else {
        CHECK(outer.left>=native.rcWork.left && outer.top>=native.rcWork.top &&
              outer.right<=native.rcWork.right && outer.bottom<=native.rcWork.bottom);
        CHECK(client.left>=native.rcWork.left && client.top>=native.rcWork.top &&
              client.right<=native.rcWork.right && client.bottom<=native.rcWork.bottom);
    }
#else
    int x=0,y=0,w=0,h=0;
    SDL_GetWindowPosition(s_vk.window,&x,&y);
    SDL_GetWindowSize(s_vk.window,&w,&h);
    CHECK(w>0 && h>0);
    if (fullscreen) {
        CHECK(x==display.x && y==display.y && w==display.w && h==display.h);
    } else {
        CHECK(x>=usable.x && y>=usable.y &&
              x+w<=usable.x+usable.w && y+h<=usable.y+usable.h);
    }
#endif
    return 0;
}
/* Check actual composed client pixels, not just the rectangle bookkeeping.
 * Run with exclusive ownership of desktop/GPU testing and an unobscured window. */
static int check_window_pixels(void)
{
#ifdef _WIN32
    SDL_SysWMinfo info;SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(s_vk.window,&info)) return 0;
    SDL_Delay(100);
    HWND window=info.info.win.window;RECT rect;
    if (!GetClientRect(window,&rect)) return 0;
    int w=rect.right,h=rect.bottom,fw=w,fh=w*9/16;
    if (fh>h) {fh=h;fw=h*16/9;}
    int fx=(w-fw)/2,fy=(h-fh)/2;
    HDC source=GetDC(window);if (!source) return 0;
    HDC dc=CreateCompatibleDC(source);
    HBITMAP bitmap=CreateCompatibleBitmap(source,w,h);
    if (!dc || !bitmap) { if(dc)DeleteDC(dc);if(bitmap)DeleteObject(bitmap);ReleaseDC(window,source);return 0; }
    HGDIOBJ previous=SelectObject(dc,bitmap);
    int captured=PrintWindow(window,dc,3 /* client only, GPU full content */);
    if (!captured) {
        SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window,source);return 0;
    }
    int ok=1;
    int xs[]={5,w/2,w-6},ys[]={5,h/2,h-6};
    for (unsigned y=0;y<3;++y) for (unsigned x=0;x<3;++x) {
        COLORREF pixel=GetPixel(dc,xs[x],ys[y]);
        int red=xs[x]>=fx && xs[x]<fx+fw && ys[y]>=fy && ys[y]<fy+fh;
        if (pixel==CLR_INVALID || (red ? GetRValue(pixel)<240 : GetRValue(pixel)>10) ||
            GetGValue(pixel)>10 || GetBValue(pixel)>10) {
            fprintf(stderr,"window pixel %d,%d (%dx%d): RGB %u,%u,%u expected %s\n",
                xs[x],ys[y],w,h,GetRValue(pixel),GetGValue(pixel),GetBValue(pixel),red?"red":"black");
            ok=0;
        }
    }
    SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window,source);return ok;
#else
    /* Pixel capture still needs a compositor-specific integration on Linux. */
    return 1;
#endif
}
int main(void)
{
    SDL_SetMainReady();env("PS3RECOMP_VK_WINDOW","1");
    env("PS3RECOMP_VK_GUEST_PROGRAMS","1");env("PS3RECOMP_RENDER_SCALE","1");
    env("PS3RECOMP_UPSCALER","off");env("PS3RECOMP_ANTIALIASING","off");
    env("PS3RECOMP_VK_FULLSCREEN","0");env("PS3RECOMP_VK_DISPLAY","0");
    CHECK(rsx_vulkan_backend_init(640,270,"Display regression")==0);
    CHECK(s_vk.window && (SDL_GetWindowFlags(s_vk.window)&SDL_WINDOW_RESIZABLE));
    CHECK(pump()==0);
    u32 color=vk_eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,320,180,NULL,0);
    CHECK(color);
    const float red[4]={1,0,0,1};vk_eng_clear_color(NULL,color,red);
    CHECK(vk_prepare_window_present());vk_eng_present(NULL,color);
    CHECK(s_vk.content_rect.x==80 && s_vk.content_rect.y==0 &&
          s_vk.content_rect.w==480 && s_vk.content_rect.h==270);
    CHECK(check_window_pixels());
    u32 widths[]={640,800,720},heights[]={360,450,320};
    for(unsigned i=0;i<3;++i){
        SDL_SetWindowSize(s_vk.window,(int)widths[i],(int)heights[i]);
        CHECK(pump()==0);CHECK(s_vk.swapchain_dirty);
        CHECK(vk_prepare_window_present());vk_eng_present(NULL,color);
        int w=0,h=0;SDL_Vulkan_GetDrawableSize(s_vk.window,&w,&h);
        CHECK(s_vk.sc_extent.width==(u32)w && s_vk.sc_extent.height==(u32)h);
        CHECK(s_vk.width==640 && s_vk.height==270); /* no hidden RT reallocation */
        CHECK(check_window_pixels());
    }
    CHECK(toggle()==0);CHECK(SDL_GetWindowFlags(s_vk.window)&SDL_WINDOW_FULLSCREEN_DESKTOP);
    CHECK(vk_prepare_window_present());vk_eng_present(NULL,color);
    CHECK(check_window_pixels());
    CHECK(toggle()==0);CHECK(!(SDL_GetWindowFlags(s_vk.window)&SDL_WINDOW_FULLSCREEN_DESKTOP));
    CHECK(vk_prepare_window_present());vk_eng_present(NULL,color);
    CHECK(check_window_pixels());
    SDL_MinimizeWindow(s_vk.window);CHECK(pump()==0);
    CHECK(!vk_prepare_window_present() && s_vk.present_deferred);
    SDL_RestoreWindow(s_vk.window);CHECK(pump()==0);
    CHECK(vk_prepare_window_present());vk_eng_present(NULL,color);
    CHECK(s_vk.successful_window_presents>=7);
    const int display_count=SDL_GetNumVideoDisplays();
    CHECK(display_count>0);
    rsx_vulkan_backend_shutdown();
    /* A 3440x1440 canvas must open wholly within each selected display.
     * Check native Windows rectangles because SDL client size alone misses
     * title bars and resize borders that can cross a monitor edge. */
    for(int selected=0;selected<display_count;++selected) {
        char index[16];snprintf(index,sizeof index,"%d",selected);
        env("PS3RECOMP_VK_DISPLAY",index);
        env("PS3RECOMP_VK_FULLSCREEN","0");
        CHECK(rsx_vulkan_backend_init(3440,1440,"Monitor containment")==0);
        CHECK(pump()==0 && check_monitor_layout(selected,0)==0);
        CHECK(toggle()==0);
        CHECK(SDL_GetWindowFlags(s_vk.window)&SDL_WINDOW_FULLSCREEN_DESKTOP);
        CHECK(check_monitor_layout(selected,1)==0);
        CHECK(toggle()==0);
        CHECK(check_monitor_layout(selected,0)==0);
        rsx_vulkan_backend_shutdown();
        env("PS3RECOMP_VK_FULLSCREEN","1");
        CHECK(rsx_vulkan_backend_init(3440,1440,"Startup fullscreen containment")==0);
        CHECK(pump()==0);
        CHECK(SDL_GetWindowFlags(s_vk.window)&SDL_WINDOW_FULLSCREEN_DESKTOP);
        CHECK(check_monitor_layout(selected,1)==0);
        CHECK(toggle()==0);
        CHECK(check_monitor_layout(selected,0)==0);
        rsx_vulkan_backend_shutdown();
    }
    char invalid[16];snprintf(invalid,sizeof invalid,"%d",display_count);
    env("PS3RECOMP_VK_DISPLAY",invalid);env("PS3RECOMP_VK_FULLSCREEN","0");
    CHECK(rsx_vulkan_backend_init(3440,1440,"Invalid monitor fallback")==0);
    CHECK(pump()==0 && check_monitor_layout(0,0)==0);
    rsx_vulkan_backend_shutdown();
    env("PS3RECOMP_VK_DISPLAY","0");
    env("PS3RECOMP_VK_WINDOW","0");
    const u32 canvases[][2]={{1280,800},{3440,1440},{5120,1440},{3840,2160},{7680,4320}};
    for (unsigned i=0;i<sizeof canvases/sizeof canvases[0];++i) {
        CHECK(rsx_vulkan_backend_init(canvases[i][0],canvases[i][1],"High output regression")==0);
        color=vk_eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,320,180,NULL,0);
        CHECK(color);vk_eng_clear_color(NULL,color,red);vk_eng_present(NULL,color);
        CHECK(s_vk.last_center==0xFF0000);
        const u8* pixel=(const u8*)s_vk.readback.ptr;
        if (s_vk.content_rect.x || s_vk.content_rect.y)
            CHECK(pixel[0]==0 && pixel[1]==0 && pixel[2]==0);
        else CHECK(pixel[0]==255 && pixel[1]==0 && pixel[2]==0);
        if(canvases[i][0]==3440 || canvases[i][0]==5120) {
            vk_eng_set_present_aspect(NULL,(float)canvases[i][0]/canvases[i][1]);
            vk_eng_present(NULL,color);
            CHECK(s_vk.content_rect.x==0 && s_vk.content_rect.y==0 &&
                  s_vk.content_rect.w==canvases[i][0] && s_vk.content_rect.h==canvases[i][1]);
            pixel=(const u8*)s_vk.readback.ptr;
            CHECK(pixel[0]==255 && pixel[1]==0 && pixel[2]==0);
            vk_eng_set_present_aspect(NULL,NAN);vk_eng_present(NULL,color);
            CHECK(s_vk.present_aspect==0 && s_vk.content_rect.x>0);
            pixel=(const u8*)s_vk.readback.ptr;
            CHECK(pixel[0]==0 && pixel[1]==0 && pixel[2]==0);
        }
        rsx_vulkan_backend_shutdown();
    }
    /* Rejected before a window or Vulkan resource is created. */
    CHECK(rsx_vulkan_backend_init(8192,8192,"invalid")==-1);
    CHECK(!s_vk.window && !s_vk.device);
    rsx_vulkan_backend_shutdown();
    printf("Vulkan display: %u checks passed\n",checks);return 0;
}

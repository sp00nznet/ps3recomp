/* Display-only geometry. Does not alter guest cameras or render targets. */
#ifndef RSX_DISPLAY_GEOMETRY_H
#define RSX_DISPLAY_GEOMETRY_H
#include <stdint.h>
typedef struct rsx_display_rect { uint32_t x,y,w,h; } rsx_display_rect;
static inline int rsx_display_fit(uint32_t sw,uint32_t sh,uint32_t dw,uint32_t dh,
                                  rsx_display_rect* out)
{
    if (!sw || !sh || !dw || !dh || !out) return -1;
    uint32_t w=dw,h=dh;
    /* Compare first: never truncate a quotient larger than UINT32_MAX. */
    if ((uint64_t)sw*dh > (uint64_t)sh*dw)
        h=(uint32_t)((uint64_t)dw*sh/sw);
    else
        w=(uint32_t)((uint64_t)dh*sw/sh);
    if (!w) w=1;
    if (!h) h=1;
    *out=(rsx_display_rect){(dw-w)/2,(dh-h)/2,w,h};
    return 0;
}
static inline int rsx_display_extent_valid(uint32_t w,uint32_t h,uint32_t device_max)
{
    return w && h && w<=8192 && h<=8192 && w<=device_max && h<=device_max &&
           (uint64_t)w*h<=33554432;
}
/* Fit a requested client area into one display without enlarging it. */
static inline int rsx_display_window_size(uint32_t requested_w,uint32_t requested_h,
                                          uint32_t available_w,uint32_t available_h,
                                          uint32_t* window_w,uint32_t* window_h)
{
    rsx_display_rect fit;
    if (!window_w || !window_h ||
        rsx_display_fit(requested_w,requested_h,available_w,available_h,&fit)) return -1;
    if (requested_w<=available_w && requested_h<=available_h) {
        *window_w=requested_w;*window_h=requested_h;
    } else {
        *window_w=fit.w;*window_h=fit.h;
    }
    return 0;
}
/* SDL displays are numbered from zero; reject malformed or out-of-range
 * overrides before passing them to SDL_WINDOWPOS_CENTERED_DISPLAY. */
static inline int rsx_display_parse_index(const char* value,int count,int* out)
{
    unsigned index=0;
    if (!value || !*value || count<=0 || !out) return -1;
    for (const char* p=value;*p;++p) {
        if (*p<'0' || *p>'9') return -1;
        const unsigned digit=(unsigned)(*p-'0');
        if (index>(unsigned)(count-1)/10u ||
            index*10u+digit>(unsigned)(count-1)) return -1;
        index=index*10u+digit;
    }
    *out=(int)index;
    return 0;
}
#endif

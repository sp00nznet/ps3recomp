/* Integer internal raster scaling. Guest addresses, pitches, texture assets
 * and sample numbers retain their original meanings. Render images use host
 * pixels; transfers cross that boundary explicitly. Startup-only for now. */
static u32 s_render_scale = 1;
static int vk_eng_readback_rows_raw(u32,u32,u32,u32,u32,void*,u32);
static int vk_eng_depth_readback_raw(u32,u32,u32,u32,u32,void*,u32);
/* The game's 1280x720 two-sample world target, rendered at DLSS's input
 * extent when DLSS runs (rsx_vulkan_dlss_frame.inc decides the extent). */
static int vk_eng_quality_world(u32 w,u32 h,u32 samples,u32 *host_w,u32 *host_h)
{
    u32 iw,ih;
    if(w!=1280 || h!=720 || samples!=2 || !vk_dlss_world_extent(&iw,&ih))return 0;
    if(host_w)*host_w=iw;
    if(host_h)*host_h=ih;
    return 1;
}
static void vk_eng_bound_scale(float *x,float *y)
{
    *x=*y=(float)s_render_scale;
    u32 handle=s_eng_bound.nrt?s_eng_bound.rt[0]:s_eng_bound.depth;
    if(handle && handle<VK_ENG_MAX_OBJ){
        const vk_eng_obj *o=&s_eobj[handle];
        if(o->guest_w && o->guest_h){*x=(float)o->w/o->guest_w;*y=(float)o->h/o->guest_h;}
    }
}

static int vk_eng_resolution_config(void)
{
    const char* value = getenv("PS3RECOMP_RENDER_SCALE");
    s_render_scale = 1;
    if (!value) return 0;
    if (value[0] < '1' || value[0] > '4' || value[1]) {
        VK_LOG("PS3RECOMP_RENDER_SCALE must be an integer1..4\n");
        return -1;
    }
    s_render_scale = (u32)(value[0] - '0');
    VK_LOG("internal raster scale %ux; guest coordinates unchanged\n",s_render_scale);
    return 0;
}

static int vk_eng_scale_extent(u32* w,u32* h,u32 samples)
{
    const u32 gx=samples>1?2u:1u,gy=samples>2?2u:1u;
    if (!*w || !*h || *w>vk_max_dim()/s_render_scale/gx ||
        *h>vk_max_dim()/s_render_scale/gy) return -1;
    *w*=s_render_scale;*h*=s_render_scale;
    return 0;
}

typedef struct vk_eng_scale_region {
    u32 k,gx,gy,x,y,w,h;
} vk_eng_scale_region;

static int vk_eng_scale_region_get(const vk_eng_obj* o,u32 x,u32 y,u32 w,u32 h,
                                   vk_eng_scale_region* r)
{
    if (!o || !w || !h) return -1;
    r->k=o->scale?o->scale:1u;
    r->gx=o->grid_x?o->grid_x:1u;r->gy=o->grid_y?o->grid_y:1u;
    const u32 gw=o->w/r->k,gh=o->h/r->k;
    if (x>gw || w>gw-x || y>gh || h>gh-y) return -1;
    r->x=(x/r->gx)*r->gx*r->k;r->y=(y/r->gy)*r->gy*r->k;
    r->w=((x+w+r->gx-1u)/r->gx)*r->gx*r->k-r->x;
    r->h=((y+h+r->gy-1u)/r->gy)*r->gy*r->k-r->y;
    return r->x>o->w || r->w>o->w-r->x || r->y>o->h || r->h>o->h-r->y ? -1 : 0;
}

typedef int (*vk_eng_raw_reader)(u32,u32,u32,u32,u32,void*,u32);
/* Preserve sample lanes while crossing a fractional-resolution CPU transfer.
 * This slow path is for guest readback/upload only; world rendering stays GPU. */
static int vk_eng_fractional_transfer(u32 handle,const vk_eng_obj *o,u32 bpp,
    u32 x,u32 y,u32 w,u32 h,void *data,u32 pitch,vk_eng_raw_reader read_raw,int write)
{
    if(!data || !bpp || !w || !h || x>o->guest_w || w>o->guest_w-x ||
       y>o->guest_h || h>o->guest_h-y || (u64)w*bpp>pitch ||
       (u64)o->w*o->h*bpp>SIZE_MAX || (u64)o->w*bpp>UINT32_MAX)return -1;
    u32 gx=o->grid_x?o->grid_x:1,gy=o->grid_y?o->grid_y:1;
    u8 *pixels=(u8*)malloc((size_t)o->w*o->h*bpp);if(!pixels)return -1;
    int rc=read_raw(handle,0,0,o->w,o->h,pixels,o->w*bpp);
    if(!rc && !write)for(u32 iy=0;iy<h;iy++)for(u32 ix=0;ix<w;ix++){
        u32 lx=x+ix,ly=y+iy;
        u32 hx=(u32)(((u64)(2*(lx/gx)+1)*(o->w/gx))/(2*(o->guest_w/gx)))*gx+lx%gx;
        u32 hy=(u32)(((u64)(2*(ly/gy)+1)*(o->h/gy))/(2*(o->guest_h/gy)))*gy+ly%gy;
        memcpy((u8*)data+(size_t)iy*pitch+(size_t)ix*bpp,pixels+((size_t)hy*o->w+hx)*bpp,bpp);
    }
    if(!rc && write){
        for(u32 hy=0;hy<o->h;hy++)for(u32 hx=0;hx<o->w;hx++){
            u32 lx=(u32)(((u64)(hx/gx)*(o->guest_w/gx))/(o->w/gx))*gx+hx%gx;
            u32 ly=(u32)(((u64)(hy/gy)*(o->guest_h/gy))/(o->h/gy))*gy+hy%gy;
            if(lx>=x && lx-x<w && ly>=y && ly-y<h)
                memcpy(pixels+((size_t)hy*o->w+hx)*bpp,(u8*)data+(size_t)(ly-y)*pitch+(size_t)(lx-x)*bpp,bpp);
        }
        rc=vk_eng_upload_region(o->im.img,0,0,0,0,o->w,o->h,(size_t)o->w*bpp,o->h,pixels,o->w*bpp);
    }
    free(pixels);return rc;
}
static int vk_eng_scaled_read(u32 handle,const vk_eng_obj* o,u32 bpp,
    u32 x,u32 y,u32 w,u32 h,void* out,u32 pitch,vk_eng_raw_reader read_raw)
{
    if(o && o->guest_w)return vk_eng_fractional_transfer(handle,o,bpp,x,y,w,h,out,pitch,read_raw,0);
    vk_eng_scale_region r;
    if (!out || !bpp || (u64)w*bpp>pitch || vk_eng_scale_region_get(o,x,y,w,h,&r)) return -1;
    if (r.k==1) return read_raw(handle,x,y,w,h,out,pitch);
    const u64 row_bytes=(u64)r.w*bpp,total=row_bytes*r.h;
    if (row_bytes>UINT32_MAX || total>SIZE_MAX) return -1;
    u8* pixels=(u8*)malloc((size_t)total);
    if (!pixels) return -1;
    const int rc=read_raw(handle,r.x,r.y,r.w,r.h,pixels,(u32)row_bytes);
    if (!rc) for (u32 iy=0;iy<h;++iy) {
        const u32 ly=y+iy;
        const u32 hy=(ly/r.gy)*r.gy*r.k+(r.k/2u)*r.gy+ly%r.gy-r.y;
        for (u32 ix=0;ix<w;++ix) {
            const u32 lx=x+ix;
            const u32 hx=(lx/r.gx)*r.gx*r.k+(r.k/2u)*r.gx+lx%r.gx-r.x;
            memcpy((u8*)out+(size_t)iy*pitch+(size_t)ix*bpp,
                   pixels+(size_t)hy*(size_t)row_bytes+(size_t)hx*bpp,bpp);
        }
    }
    free(pixels);return rc;
}

static int vk_eng_scaled_write(u32 handle,vk_eng_obj* o,u32 x,u32 y,u32 w,u32 h,
                                const void* rows,u32 pitch)
{
    if(o && o->guest_w)return vk_eng_fractional_transfer(handle,o,o->bpp,x,y,w,h,(void*)rows,pitch,vk_eng_readback_rows_raw,1);
    vk_eng_scale_region r;
    if (!rows || !o || !o->bpp || (u64)w*o->bpp>pitch || vk_eng_scale_region_get(o,x,y,w,h,&r)) return -1;
    if (r.k==1) return vk_eng_upload_region(o->im.img,0,0,x,y,w,h,(size_t)w*o->bpp,h,rows,pitch);
    const u64 row_bytes=(u64)r.w*o->bpp,total=row_bytes*r.h;
    if (row_bytes>UINT32_MAX || total>SIZE_MAX) return -1;
    u8* pixels=(u8*)malloc((size_t)total);
    if (!pixels) return -1;
    /* An odd sample-grid rectangle can leave neighboring sample numbers
     * untouched. Preserve their high-resolution contents before replacing. */
    int rc=0;
    if (r.gx>1 || r.gy>1)
        rc=vk_eng_readback_rows_raw(handle,r.x,r.y,r.w,r.h,pixels,(u32)row_bytes);
    if (!rc) {
        for (u32 iy=0;iy<r.h;++iy) {
            const u32 hy=r.y+iy,ly=(hy/(r.k*r.gy))*r.gy+hy%r.gy;
            for (u32 ix=0;ix<r.w;++ix) {
                const u32 hx=r.x+ix,lx=(hx/(r.k*r.gx))*r.gx+hx%r.gx;
                if (lx>=x && lx-x<w && ly>=y && ly-y<h)
                    memcpy(pixels+(size_t)iy*(size_t)row_bytes+(size_t)ix*o->bpp,
                           (const u8*)rows+(size_t)(ly-y)*pitch+(size_t)(lx-x)*o->bpp,o->bpp);
            }
        }
        rc=vk_eng_upload_region(o->im.img,0,0,r.x,r.y,r.w,r.h,(size_t)row_bytes,r.h,pixels,(u32)row_bytes);
    }
    free(pixels);return rc;
}

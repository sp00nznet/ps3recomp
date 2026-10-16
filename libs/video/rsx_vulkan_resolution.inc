/* Integer internal raster scaling. Guest addresses, pitches, texture assets
 * and sample numbers retain their original meanings. Render images use host
 * pixels; transfers cross that boundary explicitly. Startup-only for now. */
static u32 s_render_scale = 1;
static int vk_eng_readback_rows_raw(u32,u32,u32,u32,u32,void*,u32);
static int vk_eng_depth_readback_raw(u32,u32,u32,u32,u32,void*,u32);

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
static int vk_eng_scaled_read(u32 handle,const vk_eng_obj* o,u32 bpp,
    u32 x,u32 y,u32 w,u32 h,void* out,u32 pitch,vk_eng_raw_reader read_raw)
{
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

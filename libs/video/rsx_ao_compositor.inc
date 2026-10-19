/* Narrow opt-in insertion at the world's opaque display composite. The
 * game's own shader still executes with t0 replaced; t1 and HUD stay intact. */
static float eng_ao_parameter(const char* name,float fallback)
{
    const char* value=getenv(name);char* end=NULL;
    if(!value || !*value)return fallback;
    float parsed=strtof(value,&end);
    return end && !*end && isfinite(parsed) ? parsed : -1;
}

static void eng_ao_configure(void)
{
    const char* mode=getenv("PS3RECOMP_RSX_AO");
    g.ao_radius=eng_ao_parameter("PS3RECOMP_RSX_AO_RADIUS",1.2f);
    g.ao_strength=eng_ao_parameter("PS3RECOMP_RSX_AO_STRENGTH",1.0f);
    g.ao_enabled=mode && (!strcmp(mode,"cacao") || !strcmp(mode,"rayquery")) && g.be->ambient_occlusion &&
        g.ao_radius>0 && g.ao_radius<=1000 && g.ao_strength>0 && g.ao_strength<=1;
    g.ao_logged=0;
    if(mode && *mode && strcmp(mode,"off"))
        fprintf(stderr,"[rsx engine] AO mode=%s %s; radius %.5g strength %.5g; qualified world composites only\n",
            mode,g.ao_enabled?"requested":"disabled (unsupported mode/backend or parameters)",g.ao_radius,g.ao_strength);
}

static int eng_ao_address(u32 location,u32 offset,const rsx_dsp_texture* t)
{ return location==t->location && offset==t->offset; }

static u32 eng_ao_composite(const u32* targets,u32 n_targets,u32 current_zslot,
    const rsx_be_render_state* rs,u32 tex_mask,const rsx_dsp_texture* ct,
    const rsx_dsp_texture* zt,u32 original)
{
    if(!g.ao_enabled || !g.be || !g.be->ambient_occlusion || !original ||
        !targets || n_targets!=1 || targets[0]>=g.n_surfaces || !rs || !ct || !zt ||
        !isfinite(g.ao_radius) || g.ao_radius<=0 || g.ao_radius>1000 ||
        !isfinite(g.ao_strength) || g.ao_strength<=0 || g.ao_strength>1)return original;
    if(tex_mask!=3 || rs->blend_enable || rs->depth_test || rs->depth_write ||
        rs->alpha_test_enable || rs->stencil_enable || rs->color_mask!=0x01010101u)return original;
    const eng_surface* target=&g.surfaces[targets[0]];
    if(!target->handle || target->samples!=1 || target->fmt!=RSX_BE_FMT_R8G8B8A8)return original;
    int display=0;
    for(u32 i=0;i<8;++i)if(g.display_buffers[i].valid &&
        g.display_buffers[i].location==target->location && g.display_buffers[i].offset==target->offset)display=1;
    if(!display || !ct->enabled || !zt->enabled || ct->cubemap || zt->cubemap ||
        ct->dimension!=2 || zt->dimension!=2 || ct->depth>1 || zt->depth>1 ||
        ct->mipmaps!=1 || zt->mipmaps!=1 ||
        (ct->format&RSX_TEX_FMT_BASE_MASK)!=RSX_TEX_FMT_A8R8G8B8 ||
        (zt->format&RSX_TEX_FMT_BASE_MASK)!=RSX_TEX_FMT_A8R8G8B8 ||
        eng_ao_address(ct->location,ct->offset,zt) ||
        eng_ao_address(target->location,target->offset,ct) ||
        eng_ao_address(target->location,target->offset,zt))return original;

    u32 ci=ENG_INVALID,zi=ENG_INVALID;int ctie=0,ztie=0;
    for(u32 i=0;i<g.n_surfaces;++i) {
        const eng_surface* c=&g.surfaces[i];
        if(!c->handle || !c->color_serial || !eng_ao_address(c->location,c->offset,ct))continue;
        if(ci==ENG_INVALID || c->color_serial>g.surfaces[ci].color_serial) { ci=i;ctie=0; }
        else if(c->color_serial==g.surfaces[ci].color_serial)ctie=1;
    }
    for(u32 i=0;i<g.n_zdepths;++i) {
        const eng_zdepth* z=&g.zdepths[i];
        if(!z->handle || !z->depth_serial || !eng_ao_address(z->location,z->offset,zt))continue;
        if(zi==ENG_INVALID || z->depth_serial>g.zdepths[zi].depth_serial) { zi=i;ztie=0; }
        else if(z->depth_serial==g.zdepths[zi].depth_serial)ztie=1;
    }
    if(ci==ENG_INVALID || zi==ENG_INVALID || ctie || ztie || zi==current_zslot)return original;
    const eng_surface* color=&g.surfaces[ci];const eng_zdepth* depth=&g.zdepths[zi];
    if(color->ao_invalidated || color->fmt!=RSX_BE_FMT_R8G8B8A8 ||
        color->color_serial<depth->depth_serial ||
        !rsx_ao_depth_usable(&depth->ao,g.frames,depth->depth_serial))return original;
    for(u32 i=0;i<g.n_zdepths;++i) {
        const eng_zdepth* z=&g.zdepths[i];
        if(z->handle && eng_ao_address(z->location,z->offset,ct) && z->depth_serial>color->color_serial)return original;
    }
    for(u32 i=0;i<g.n_surfaces;++i) {
        const eng_surface* c=&g.surfaces[i];
        if(c->handle && eng_ao_address(c->location,c->offset,zt) && c->color_serial>=depth->depth_serial)return original;
    }
    if(color->samples!=1 && color->samples!=2 && color->samples!=4)return original;
    const u32 gx=color->samples>1?2:1,gy=color->samples==4?2:1;
    if(color->samples!=depth->samples || !color->raster_w || !color->raster_h ||
        color->raster_w>UINT32_MAX/gx || color->raster_h>UINT32_MAX/gy ||
        color->w!=color->raster_w*gx || color->h!=color->raster_h*gy ||
        color->pitch<(u64)color->w*4u || depth->pitch<(u64)depth->w*4u ||
        depth->w!=color->w || depth->h!=color->h ||
        depth->raster_w!=color->raster_w || depth->raster_h!=color->raster_h ||
        target->raster_w!=color->raster_w || target->raster_h!=color->raster_h ||
        depth->ao.width!=color->raster_w || depth->ao.height!=color->raster_h ||
        ct->width!=color->w || ct->height!=color->h || ct->pitch!=color->pitch ||
        zt->width!=depth->w || zt->height!=depth->h || zt->pitch!=depth->pitch)return original;
    const u32 replacement=g.be->ambient_occlusion(g.be->user,color->handle,depth->handle,
        color->raster_w,color->raster_h,&depth->ao.projection.matrix[0][0],
        ct->remap&0xFFFFu,ct->format,g.ao_radius,g.ao_strength);
    if(replacement && !g.ao_logged) {
        fprintf(stderr,"[rsx engine] requested AO world composite applied frame %u logical %ux%u samples %u; original sources preserved\n",
            g.frames,color->raster_w,color->raster_h,color->samples);
        g.ao_logged=1;
    }
    return replacement?replacement:original;
}

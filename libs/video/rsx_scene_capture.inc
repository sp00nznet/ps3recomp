/* Private opt-in resource capture, called only inside an already active
 * geometry dump. A numeric mask retains the alpha-only capture behavior;
 * "all" captures every sampled fragment texture on opaque and alpha draws.
 * FTX1 is 32 LE u32 words followed by original guest texture bytes. A cube
 * payload contains six complete mip chains with 128-byte-aligned face strides,
 * in the same order as eng_texture_upload. */
#include "rsx_scene_texture_layout.h"
static void pd_alpha_textures(const rsx_be_render_state* rs,u32 draw,u32 mask)
{
    const char* selected=getenv("RSX_ENGINE_PASS_DUMP_TEXTURE_MASK");
    if(!selected || !*selected)return;
    const char* draw_filter=getenv("RSX_ENGINE_PASS_DUMP_TEXTURE_DRAW");
    if(draw_filter && *draw_filter){
        char* end=NULL;unsigned long wanted=strtoul(draw_filter,&end,10);
        if(end==draw_filter || *end || wanted>127u || draw!=(u32)wanted)return;
    }
    const int all=!strcmp(selected,"all");
    if(!all) {
        if(!rs->alpha_test_enable)return;
        char* end=NULL;unsigned long parsed=strtoul(selected,&end,0);
        if(end==selected || *end || parsed>0xffffu)return;
        mask&=(u32)parsed;
    }
    static u64 total_bytes=0;
    for(u32 unit=0;unit<16;unit++) {
        if(!(mask&(1u<<unit)))continue;
        rsx_dsp_texture t;rsx_dsp_get_texture(&g.rsx,unit,&t);
        u32 reason=0,span=0;const u8* src=NULL;u64 hash=0;
        if(!t.enabled || !t.width || !t.height || t.dimension!=2 || t.depth>1 ||
           (t.cubemap && t.width!=t.height) || t.width>ENG_MAX_TEX_DIM ||
           t.height>ENG_MAX_TEX_DIM || t.mipmaps>RSX_MAX_TEXTURE_LEVELS)reason=1;
        if(!reason)switch(t.format&0x9fu) {
        case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86:
        case 0x87: case 0x88: case 0x8b: case 0x8c: case 0x8d: case 0x90:
        case 0x92: case 0x94: case 0x95: case 0x97: case 0x9a: case 0x9b:
        case 0x9c: case 0x9d: case 0x9e: case 0x9f: break;
        default: reason=5;break;
        }
        /* Sampling a live GPU target is not equivalent to reading stale guest
         * memory. Record rejection rather than manufacturing texture pixels. */
        for(u32 i=0;i<g.n_surfaces;i++)
            if(g.surfaces[i].handle && g.surfaces[i].location==t.location && g.surfaces[i].offset==t.offset)reason=2;
        for(u32 i=0;i<g.n_zdepths;i++)
            if(g.zdepths[i].handle && g.zdepths[i].location==t.location && g.zdepths[i].offset==t.offset)reason=2;
        if(!reason) {
            span=rsx_scene_texture_capture_span(t.format,t.width,t.height,
                                                t.mipmaps,t.pitch,t.cubemap!=0);
            /* The independent checked span must match the uploader's source
             * range before any guest read or capture is allowed. */
            if(span && span!=eng_texture_span(t.format,t.width,t.height,
                                             t.mipmaps,t.pitch,t.cubemap!=0))span=0;
            if(!span || total_bytes+span>128u*1024u*1024u)reason=3;
        }
        if(!reason) {
            int readable=0;
            hash=eng_texture_content_hash(t.location,t.offset,span,&readable);
            src=readable?eng_guest_ptr(NULL,t.location,t.offset,span):NULL;
            int matched=0;
            for(u32 i=0;i<g.n_textures;i++) {
                const eng_texture* e=&g.textures[i];
                if(e->handle && e->location==t.location && e->offset==t.offset &&
                   e->format==t.format && e->width==t.width && e->height==t.height &&
                   e->levels==t.mipmaps && e->pitch==t.pitch && e->remap==(t.remap&0xffffu) &&
                   e->cubemap==(u32)(t.cubemap!=0) &&
                   e->content_hash==hash)matched=1;
            }
            if(!src || !matched)reason=4;
        }
        /* Fields: magic/version/pass/draw/unit, location/offset/format,
         * dimension/cubemap/mips/width/height/pitch/depth, wrap/remap/filter/
         * control0/border, payload bytes/rejection reason, decoder RGBA flag,
         * alpha function/reference raw/reference format, source hash lo/hi. */
        u32 h[32]={0x31585446u,1,s_pd.pass,draw,unit,t.location,t.offset,t.format,
            t.dimension,t.cubemap,t.mipmaps,t.width,t.height,t.pitch,t.depth,
            t.wrap,t.remap,t.filter,t.control0,t.border_color,reason?0:span,reason,
            (u32)rsx_texture_argb_is_rgba(),rs->alpha_func,rs->alpha_ref_raw,
            rs->alpha_ref_format,(u32)hash,(u32)(hash>>32)};
        char path[1024];int n=snprintf(path,sizeof path,"%s/pass%03u_draw%03u_tex%02u.ftx",s_pd.dir,s_pd.pass,draw,unit);
        if(n<0 || (size_t)n>=sizeof path)continue;
        FILE* f=fopen(path,"wb");if(!f)continue;
        int ok=fwrite(h,1,sizeof h,f)==sizeof h && (reason || fwrite(src,1,span,f)==span);
        if(fclose(f))ok=0;
        if(!reason)total_bytes+=span;
        if(!ok || reason)fprintf(stderr,"[rsx scene capture] texture pass%u draw%u unit%u rejected=%u write_ok=%d\n",s_pd.pass,draw,unit,reason,ok);
    }
}

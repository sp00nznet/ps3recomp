/* Opt-in Folklore presentation/HUD classifier. A world surface and its
 * two-texture resolve establish content provenance independent of HUD draws.
 * It never changes guest constants or world geometry.
 * The captured HUD is affine, depth-test-free geometry on the
 * final 1280x720 surface; movies and full-screen composites do not qualify. */
/* Identify the source image, not merely another world draw somewhere in the
 * frame. A shadow or menu preview cannot make an unrelated flip ultrawide. */
static int eng_folklore_sampled_world(u32 tex_mask, int scene_only)
{
    for (u32 unit=0;unit<16;++unit) {
        if (!(tex_mask & (1u<<unit))) continue;
        rsx_dsp_texture texture;
        rsx_dsp_get_texture(&g.rsx,unit,&texture);
        for (u32 i=0;i<g.n_surfaces;++i) {
            const eng_surface* source=&g.surfaces[i];
            if (source->handle && source->location==texture.location &&
                source->offset==texture.offset && source->folklore_world &&
                (!scene_only || source->folklore_world==1)) return 1;
        }
    }
    return 0;
}

/* Require two actual triangles covering a rectangle, not just a bounding
 * box: a large triangle or unrelated sprites can have full-screen bounds. */
static int eng_folklore_rectangle(const float matrix[4][4], unsigned slot,
                                  const double low[2],const double high[2],
                                  const u32* indices,u32 count,int strip)
{
    if (strip<0 || count!=(strip?4u:6u) || dc.n_verts>6) return 0;
    unsigned corners[6];
    for (u32 i=0;i<dc.n_verts;++i) {
        float v[4];double xy[2]={0,0};
        memcpy(v,dc.verts+(size_t)i*dc.layout.stride+slot*16u,sizeof v);
        for(unsigned row=0;row<2;++row)
            for(unsigned col=0;col<4;++col)xy[row]+=(double)matrix[row][col]*v[col];
        unsigned corner=0;
        for(unsigned row=0;row<2;++row) {
            if(fabs(xy[row]-low[row])<0.0001) continue;
            if(fabs(xy[row]-high[row])>=0.0001) return 0;
            corner|=1u<<row;
        }
        corners[i]=corner;
    }
    unsigned masks[2]={0,0};int winding[2];
    for(unsigned tri=0;tri<2;++tri) {
        unsigned c[3];
        for(unsigned k=0;k<3;++k) {
            const unsigned ref=strip ? tri+k : tri*3u+k;
            const u32 vertex=indices?indices[ref]:ref;
            if(vertex>=dc.n_verts)return 0;
            c[k]=corners[vertex];masks[tri]|=1u<<c[k];
        }
        const int ax=(int)(c[1]&1u)-(int)(c[0]&1u),ay=(int)(c[1]>>1)-(int)(c[0]>>1);
        const int bx=(int)(c[2]&1u)-(int)(c[0]&1u),by=(int)(c[2]>>1)-(int)(c[0]>>1);
        winding[tri]=(ax*by-ay*bx)*(strip && tri ? -1 : 1);
    }
    const unsigned diagonal=masks[0]&masks[1];
    return (masks[0]|masks[1])==15u && (diagonal==9u || diagonal==6u) &&
        winding[0] && winding[0]==winding[1];
}

static float eng_folklore_present_aspect(const eng_surface* rt)
{
    return rt->folklore_world==2 ? g.world_aspect : 0;
}

static int eng_folklore_hud_check(const rsx_be_render_state* rs,eng_surface* rt,
                                  const eng_pipeline* pipeline,u32 tex_mask,const u32* indices,u32 count,int strip,int* right,int* bottom,
                                  double low[2],double high[2],const char** why)
{
    float matrix[4][4];
    memcpy(matrix,g.rsx.constants+256,sizeof matrix);
    if (rt->raster_w!=1280 || rt->raster_h!=720) { *why="size"; return 0; }
    if (rt->samples==2 && rs->depth_test && (rs->color_mask&0xFFFFFFu) &&
        (matrix[3][0]!=0 || matrix[3][1]!=0 || matrix[3][2]!=0)) {
        rt->folklore_world=1;
        g.hud_world_seen=1;
    }
    if (rt->samples!=1 || rs->depth_test || !rs->color_mask) { *why="depth"; return 0; }
    if (!rs->alpha_test_enable && !rs->blend_enable && !rs->stencil_enable &&
        (rs->color_mask&0xFFu) && (rs->color_mask&0xFF00u) && (rs->color_mask&0xFF0000u) &&
        (tex_mask&3u)==3u && eng_folklore_full_target(rt)) {
        rsx_dsp_texture color,depth;
        rsx_dsp_get_texture(&g.rsx,0,&color);rsx_dsp_get_texture(&g.rsx,1,&depth);
        if (color.width==2560 && color.height==720 &&
            depth.width==2560 && depth.height==720 &&
            eng_folklore_sampled_world(1u,1)) {
            rt->folklore_world=2;
            g.hud_composite_target=rt->handle;
            *why="resolve";
            return 0;
        }
    }
    if (rt->folklore_world!=2 && !eng_folklore_sampled_world(tex_mask,0)) { *why="target"; return 0; }
    if (matrix[3][0]!=0 || matrix[3][1]!=0 || matrix[3][2]!=0 || matrix[3][3]!=1) { *why="matrix"; return 0; }
    if (!dc.n_verts || dc.n_verts>4096) { *why="verts"; return 0; }
    unsigned slot=0;
    while (slot<dc.layout.count && dc.layout.attrs[slot]!=0) ++slot;
    if (slot==dc.layout.count) { *why="layout"; return 0; }
    low[0]=low[1]=1e30;high[0]=high[1]=-1e30;
    for (u32 i=0;i<dc.n_verts;++i) {
        float v[4];memcpy(v,dc.verts+(size_t)i*dc.layout.stride+slot*16u,sizeof v);
        if(!isfinite(v[3]) || fabsf(v[3]-1.0f)>0.0001f) { *why="homogeneous"; return 0; }
        for (unsigned row=0;row<2;++row) {
            double value=0;
            for(unsigned col=0;col<4;++col)value+=(double)matrix[row][col]*v[col];
            if(!isfinite(value)) { *why="nan"; return 0; }
            if(value<low[row])low[row]=value;
            if(value>high[row])high[row]=value;
        }
    }
    const int fullscreen=low[0]<=-0.999 && high[0]>=0.999 && low[1]<=-0.999 && high[1]>=0.999;
    /* An opaque textured full-target replacement establishes new content.
     * Solid flashes and blended fades/tutorials retain world presentation. Copies of
     * a resolved scene inherit its provenance, including alternate flip buffers.
     * The caller verifies viewport/scissor coverage before this reset. */
    if (fullscreen && tex_mask && !rs->alpha_test_enable && !rs->blend_enable &&
        !rs->stencil_enable && (rs->color_mask&0xFFu) &&
        (rs->color_mask&0xFF00u) && (rs->color_mask&0xFF0000u) && eng_folklore_full_target(rt) &&
        eng_folklore_rectangle(matrix,slot,low,high,indices,count,strip)) {
        rt->folklore_world=eng_folklore_sampled_world(tex_mask,0) ? 2 : 0;
        if (rt->folklore_world==2) g.hud_composite_target=rt->handle;
        *why="replacement";
        return 0;
    }
    if (rt->folklore_world!=2) { *why="target"; return 0; }
    if (!rs->alpha_test_enable && !rs->blend_enable) { *why="state"; return 0; }
    if (low[0]<=-0.95 && high[0]>=0.95 && low[1]<=-0.95 && high[1]>=0.95) { *why="fullscreen"; return 0; }
    /* The resolved world's provenance admits prompts even before the status
     * bar, or while it is hidden. The marker remains diagnostic only. */
    const int marker=pipeline && pipeline->folklore_hud_marker;
    if(marker){g.hud_active=1;g.hud_marker_seen=1;}
    *why=NULL;
    *right=rsx_hud_anchor_clip(low[0],high[0],0);
    *bottom=rsx_hud_anchor_clip(low[1],high[1],1);
    return 1;
}

/* RSX_ENGINE_HUD_LOG=<n> (diagnostic): the first n draws into the final
 * composite target, with their state and whether they were treated as HUD. */
static int eng_folklore_hud(const rsx_be_render_state* rs,eng_surface* rt,
                            const eng_pipeline* pipeline,u32 tex_mask,const u32* indices,u32 count,int strip,int* right,int* bottom)
{
    static int left=-1;
    if (left<0) { const char* e=getenv("RSX_ENGINE_HUD_LOG"); left=e?atoi(e):0; }
    double low[2]={0,0},high[2]={0,0};
    const char* why=NULL;
    const int hud=eng_folklore_hud_check(rs,rt,pipeline,tex_mask,indices,count,strip,right,bottom,low,high,&why);
    if (left>0 && g.hud_composite_target && rt->handle==g.hud_composite_target) {
        left--;
        fprintf(stderr,"[rsx HUD] frame=%u hud=%d why=%s blend=%d alpha=%d depth=%d verts=%u tex=0x%X "
                "marker=%d active=%d x=[%.3f,%.3f] y=[%.3f,%.3f]\n",
                g.frames,hud,why?why:"-",rs->blend_enable,rs->alpha_test_enable,
                rs->depth_test,dc.n_verts,tex_mask,pipeline&&pipeline->folklore_hud_marker,g.hud_active,
                low[0],high[0],low[1],high[1]);
    }
    return hud;
}

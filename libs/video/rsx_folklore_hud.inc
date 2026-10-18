/* Opt-in Folklore HUD transform. A world frame and its two-texture resolve
 * must precede the overlay. It never changes guest constants or world draws.
 * The captured HUD is affine, alpha-tested, depth-test-free geometry on the
 * final 1280x720 surface; movies and full-screen composites do not qualify. */
static int eng_folklore_hud_check(const rsx_be_render_state* rs,const eng_surface* rt,
                                  const eng_pipeline* pipeline,u32 tex_mask,int* right,int* bottom,
                                  double low[2],double high[2],const char** why)
{
    float matrix[4][4];
    memcpy(matrix,g.rsx.constants+256,sizeof matrix);
    if (rt->raster_w!=1280 || rt->raster_h!=720) { *why="size"; return 0; }
    if (rt->samples==2 && rs->depth_test &&
        (matrix[3][0]!=0 || matrix[3][1]!=0 || matrix[3][2]!=0))
        g.hud_world_seen=1;
    if (g.hud_world_seen && rt->samples==1 && !rs->depth_test &&
        !rs->alpha_test_enable && !rs->blend_enable && (tex_mask&3u)==3u) {
        rsx_dsp_texture color,depth;
        rsx_dsp_get_texture(&g.rsx,0,&color);rsx_dsp_get_texture(&g.rsx,1,&depth);
        if (color.width==2560 && color.height==720 &&
            depth.width==2560 && depth.height==720)
            g.hud_composite_target=rt->handle;
    }
    if (!g.hud_composite_target || rt->handle!=g.hud_composite_target) { *why="target"; return 0; }
    if (rt->samples!=1 || rs->depth_test) { *why="depth"; return 0; }
    if (!rs->alpha_test_enable || !rs->blend_enable) { *why="state"; }
    else if (matrix[3][0]!=0 || matrix[3][1]!=0 || matrix[3][2]!=0 || matrix[3][3]!=1) { *why="matrix"; return 0; }
    else if (!dc.n_verts || dc.n_verts>4096) { *why="verts"; return 0; }
    else *why=NULL;
    /* Anchor each connected HUD group at its nearest screen corner. The
     * minimap and status bars retain their corner placement when reduced. */
    unsigned slot=0;
    while (slot<dc.layout.count && dc.layout.attrs[slot]!=0) ++slot;
    if (slot==dc.layout.count || !dc.n_verts) { *why="layout"; return 0; }
    low[0]=low[1]=1e30;high[0]=high[1]=-1e30;
    for (u32 i=0;i<dc.n_verts;++i) {
        float v[4];memcpy(v,dc.verts+(size_t)i*dc.layout.stride+slot*16u,sizeof v);
        for (unsigned row=0;row<2;++row) {
            double value=0;
            for(unsigned col=0;col<4;++col)value+=(double)matrix[row][col]*v[col];
            if(!isfinite(value)) { *why="nan"; return 0; }
            if(value<low[row])low[row]=value;
            if(value>high[row])high[row]=value;
        }
    }
    if (*why) return 0;
    /* Require a positive gameplay HUD marker, not merely generic 2D state.
     * Menu/world viewers can use the same final resolve and affine draws. */
    const int marker=pipeline && pipeline->folklore_hud_marker;
    if (!g.hud_active && !marker) { *why="marker"; return 0; }
    /* Alpha-tested fades or screen overlays are not HUD elements. Reject
     * geometry covering the central 95% of both clip-space axes. */
    if (low[0]<=-0.95 && high[0]>=0.95 && low[1]<=-0.95 && high[1]>=0.95) { *why="fullscreen"; return 0; }
    if(marker)g.hud_active=1;
    *right=low[0]+high[0]>=0;
    *bottom=low[1]+high[1]<0;
    return 1;
}

/* RSX_ENGINE_HUD_LOG=<n> (diagnostic): the first n draws into the final
 * composite target, with their state and whether they were treated as HUD. */
static int eng_folklore_hud(const rsx_be_render_state* rs,const eng_surface* rt,
                            const eng_pipeline* pipeline,u32 tex_mask,int* right,int* bottom)
{
    static int left=-1;
    if (left<0) { const char* e=getenv("RSX_ENGINE_HUD_LOG"); left=e?atoi(e):0; }
    double low[2]={0,0},high[2]={0,0};
    const char* why=NULL;
    const int hud=eng_folklore_hud_check(rs,rt,pipeline,tex_mask,right,bottom,low,high,&why);
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

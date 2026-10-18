/* Opt-in Folklore HUD transform. A world frame and its two-texture resolve
 * must precede the overlay. It never changes guest constants or world draws.
 * The captured HUD is affine, alpha-tested, depth-test-free geometry on the
 * final 1280x720 surface; movies and full-screen composites do not qualify. */
static int eng_folklore_hud(const rsx_be_render_state* rs,const eng_surface* rt,
                            const eng_pipeline* pipeline,u32 tex_mask,int* right,int* bottom)
{
    float matrix[4][4];
    memcpy(matrix,g.rsx.constants+256,sizeof matrix);
    if (rt->raster_w!=1280 || rt->raster_h!=720) return 0;
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
    if (!g.hud_composite_target || rt->handle!=g.hud_composite_target ||
        rt->samples!=1 || rs->depth_test || !rs->alpha_test_enable ||
        !rs->blend_enable || matrix[3][0]!=0 || matrix[3][1]!=0 ||
        matrix[3][2]!=0 || matrix[3][3]!=1 || !dc.n_verts || dc.n_verts>4096)
        return 0;
    /* Require a positive gameplay HUD marker, not merely generic 2D state.
     * Menu/world viewers can use the same final resolve and affine draws. */
    const int marker=pipeline && pipeline->folklore_hud_marker;
    if (!g.hud_active && !marker) return 0;
    /* Anchor each connected HUD group at its nearest screen corner. The
     * minimap and status bars retain their corner placement when reduced. */
    unsigned slot=0;
    while (slot<dc.layout.count && dc.layout.attrs[slot]!=0) ++slot;
    if (slot==dc.layout.count) return 0;
    double low[2]={1e30,1e30},high[2]={-1e30,-1e30};
    for (u32 i=0;i<dc.n_verts;++i) {
        float v[4];memcpy(v,dc.verts+(size_t)i*dc.layout.stride+slot*16u,sizeof v);
        for (unsigned row=0;row<2;++row) {
            double value=0;
            for(unsigned col=0;col<4;++col)value+=(double)matrix[row][col]*v[col];
            if(!isfinite(value))return 0;
            if(value<low[row])low[row]=value;
            if(value>high[row])high[row]=value;
        }
    }
    /* Alpha-tested fades or screen overlays are not HUD elements. Reject
     * geometry covering the central 95% of both clip-space axes. */
    if (low[0]<=-0.95 && high[0]>=0.95 && low[1]<=-0.95 && high[1]>=0.95)
        return 0;
    if(marker)g.hud_active=1;
    *right=low[0]+high[0]>=0;
    *bottom=low[1]+high[1]<0;
    return 1;
}

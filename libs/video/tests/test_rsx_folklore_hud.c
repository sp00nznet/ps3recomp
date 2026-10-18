/* Exercise the draw engine's actual Folklore classifier without a GPU. */
#include "../rsx_hud_geometry.h"
#include "../rsx_draw_engine.h"
#include "../rsx_dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { u32 raster_w, raster_h, samples, handle, location, offset; int folklore_world; } eng_surface;
typedef struct { int folklore_hud_marker; } eng_pipeline;
static struct {
    rsx_dispatch rsx;
    int hud_world_seen, hud_active, hud_marker_seen;
    float world_aspect;
    u32 hud_composite_target, frames, n_surfaces;
    eng_surface surfaces[3];
} g;
static struct {
    u32 n_verts;
    unsigned char *verts;
    struct { unsigned count, stride, attrs[1]; } layout;
} dc;
void rsx_dsp_get_texture(const rsx_dispatch *rsx, u32 unit,
                                rsx_dsp_texture *out)
{
    (void)rsx; (void)unit;
    *out=(rsx_dsp_texture){.width=2560,.height=720,.location=0,.offset=unit==0 ? 100 : 200};
}
static const u32 quad_indices[6]={0,1,2,2,1,3};
static int full_target=1;
static int eng_folklore_full_target(const eng_surface* rt) { (void)rt;return full_target; }
#include "../rsx_folklore_hud.inc"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); failures++; } } while (0)
static void matrix(int world)
{
    float m[4][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    if (world) m[3][0]=1;
    memcpy(g.rsx.constants+256,m,sizeof m);
}
static void quad(float x0,float y0,float x1,float y1)
{
    static float v[4][4];
    const float coords[4][2]={{x0,y0},{x1,y0},{x0,y1},{x1,y1}};
    for(unsigned i=0;i<4;i++) { v[i][0]=coords[i][0];v[i][1]=coords[i][1];v[i][2]=0;v[i][3]=1; }
    dc.verts=(unsigned char*)v; dc.n_verts=4;
    dc.layout.count=1;dc.layout.stride=16;dc.layout.attrs[0]=0;
}
int main(void)
{
    g.world_aspect=3440.0f/1440.0f;
    g.n_surfaces=3;
    g.surfaces[0]=(eng_surface){1280,720,2,11,0,100,0};
    g.surfaces[1]=(eng_surface){1280,720,1,55,0,300,0};
    g.surfaces[2]=(eng_surface){1280,720,1,66,0,400,0};
    eng_surface *world=&g.surfaces[0], *final=&g.surfaces[1], *other_final=&g.surfaces[2];
    rsx_be_render_state depth={.depth_test=1,.color_mask=0x01010101}, resolve={.color_mask=0x01010101};
    rsx_be_render_state blended={.blend_enable=1,.color_mask=0x01010101}, alpha={.alpha_test_enable=1,.color_mask=0x01010101};
    eng_pipeline marker={1}, other={0};
    int right=0,bottom=0;
    double low[2],high[2];const char *why=NULL;
    quad(0.7f,0.7f,0.9f,0.9f);matrix(0);
    CHECK(!eng_folklore_hud_check(&blended,final,&marker,1,quad_indices,6,0,&right,&bottom,low,high,&why));
    CHECK(final->folklore_world==0 && eng_folklore_present_aspect(final)==0); /* native menu/movie, even with a HUD-like shader */
    matrix(1);
    CHECK(!eng_folklore_hud_check(&depth,world,&other,0,quad_indices,6,0,&right,&bottom,low,high,&why));
    CHECK(world->folklore_world==1 && g.hud_world_seen);
    matrix(0);
    CHECK(!eng_folklore_hud_check(&resolve,final,&other,3,quad_indices,6,0,&right,&bottom,low,high,&why));
    CHECK(g.hud_composite_target==55 && final->folklore_world==2);
    /* Prompt before the status marker: classification must not wait for the health bar. */
    quad(-0.2f,-0.2f,0.2f,0.2f);
    CHECK(eng_folklore_hud_check(&blended,final,&other,1,quad_indices,6,0,&right,&bottom,low,high,&why));
    CHECK(right==2 && bottom==2 && !g.hud_active);
    quad(0.7f,0.7f,0.9f,0.9f);
    CHECK(eng_folklore_hud(&alpha,final,&marker,1,quad_indices,6,0,&right,&bottom));
    CHECK(right==1 && bottom==0 && g.hud_marker_seen);
    /* Repeated flips/hidden HUD retain the *surface*, independent of frame-local markers. */
    g.hud_composite_target=0;g.hud_world_seen=0;g.hud_active=0;g.hud_marker_seen=0;
    CHECK(final->folklore_world==2 && eng_folklore_present_aspect(final)==g.world_aspect);
    quad(-0.2f,-0.2f,0.2f,0.2f);
    CHECK(eng_folklore_hud_check(&alpha,final,&other,1,quad_indices,6,0,&right,&bottom,low,high,&why));
    CHECK(!eng_folklore_hud_check(&alpha,other_final,&other,1,quad_indices,6,0,&right,&bottom,low,high,&why));
    /* Blended tutorial/fade over world keeps world provenance and is not HUD-scaled. */
    quad(-1,-1,1,1);
    CHECK(!eng_folklore_hud_check(&blended,final,&other,1,quad_indices,6,0,&right,&bottom,low,high,&why));
    CHECK(final->folklore_world==2 && eng_folklore_present_aspect(final)==g.world_aspect);
    /* Opaque replacement without a world source is native; cropped draws cannot reset it. */
    CHECK(!eng_folklore_hud_check(&resolve,final,&other,0,quad_indices,6,0,&right,&bottom,low,high,&why));
    CHECK(final->folklore_world==2); /* solid full-screen flash */
    full_target=0;
    CHECK(!eng_folklore_hud_check(&resolve,final,&other,2,quad_indices,6,0,&right,&bottom,low,high,&why));
    CHECK(final->folklore_world==2 && eng_folklore_present_aspect(final)==g.world_aspect);
    full_target=1;
    /* Full bounds alone are insufficient: duplicated triangle, partial write,
     * and stencil must retain the scene. */
    CHECK(!eng_folklore_hud_check(&resolve,final,&other,2,quad_indices,6,-1,&right,&bottom,low,high,&why));
    CHECK(final->folklore_world==2);
    const u32 duplicate[6]={0,1,2,0,1,2};
    CHECK(!eng_folklore_hud_check(&resolve,final,&other,2,duplicate,6,0,&right,&bottom,low,high,&why));
    CHECK(final->folklore_world==2);
    resolve.color_mask=0xFF000000u;
    CHECK(!eng_folklore_hud_check(&resolve,final,&other,2,quad_indices,6,0,&right,&bottom,low,high,&why));
    CHECK(final->folklore_world==2);
    resolve.color_mask=0x01010101u;resolve.stencil_enable=1;
    CHECK(!eng_folklore_hud_check(&resolve,final,&other,2,quad_indices,6,0,&right,&bottom,low,high,&why));
    CHECK(final->folklore_world==2);
    resolve.stencil_enable=0;
    full_target=1;
    CHECK(!eng_folklore_hud_check(&resolve,final,&other,2,quad_indices,6,0,&right,&bottom,low,high,&why));
    CHECK(final->folklore_world==0 && eng_folklore_present_aspect(final)==0);
    final->folklore_world=2;
    CHECK(!eng_folklore_hud_check(&resolve,final,&other,2,NULL,4,1,&right,&bottom,low,high,&why));
    CHECK(eng_folklore_present_aspect(final)==0);
    /* Stale frame-global world flags cannot turn an unrelated menu resolve wide. */
    world->folklore_world=0;g.hud_world_seen=1;
    CHECK(!eng_folklore_hud_check(&resolve,other_final,&other,3,quad_indices,6,0,&right,&bottom,low,high,&why));
    CHECK(other_final->folklore_world==0);
    /* Return to gameplay with no status-bar draw. */
    matrix(1);
    CHECK(!eng_folklore_hud_check(&depth,world,&other,0,quad_indices,6,0,&right,&bottom,low,high,&why));
    matrix(0);
    CHECK(!eng_folklore_hud_check(&resolve,other_final,&other,3,quad_indices,6,0,&right,&bottom,low,high,&why));
    CHECK(other_final->folklore_world==2 && final->folklore_world==0);
    printf("test_rsx_folklore_hud: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}

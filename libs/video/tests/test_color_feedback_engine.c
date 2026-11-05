/* Production binding/descriptor/lifetime checks; synthetic addresses only. */
#define main engine_retention_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"
static rsx_draw_backend feedback_backend;
static unsigned checks, failures, copies;
static u32 source_seen, snapshot_seen;
static int fail_copy, fail_view;
#define T(c,msg) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",msg);}}while(0)
static u32 snapshot(void* user,u32 src){(void)user;copies++;source_seen=src;return fail_copy?0:(snapshot_seen=stub_handle());}
static u32 view(void* user,u32 src,u32 remap,u32 fmt){return fail_view?0:stub_surface_view(user,src,remap,fmt);}
static void up(void){engine_up();feedback_backend=g_stub_backend;rsx_draw_engine_set_backend(&feedback_backend);copies=0;fail_copy=fail_view=0;feedback_backend.color_snapshot=snapshot;feedback_backend.surface_view=view;m(M_SURFACE_FORMAT,0x148);m(M_COLOR_A_PITCH,1024);bind_texture_unit(RSX_LOCATION_LOCAL,0x40000);m(M_TEX_UNIT+0x18,(256<<16)|256);m(0x1840,1024);}
static void down(void){engine_down();feedback_backend.color_snapshot=NULL;feedback_backend.surface_view=stub_surface_view;}
static void rejection(u32 method,u32 value){up();m(method,value);draw_triangle();T(!stub.n_draw&&copies==0,"unsupported layout rejects before draw or copy");down();}
int main(void){vm_base=calloc(1,GUEST_BYTES);ppu_vm_size=GUEST_BYTES;
 up();draw_triangle();T(copies==1&&stub.n_draw==1,"own target snapshots before emitted draw");T(source_seen==stub.bound_surface&&snapshot_seen==stub.bound_tex[0]&&source_seen!=snapshot_seen,"own target binds distinct copied texture");T(stub.n_texture_create==0,"own target avoids stale guest upload");u32 prior=snapshot_seen;draw_triangle();T(copies==2&&snapshot_seen!=prior&&!stub.alive[prior],"next draw has fresh copy and retires old one");u32 last=snapshot_seen;down();T(!stub.alive[last],"shutdown releases last copy");
 up();m(M_COLOR_A_OFFSET,0x50000);draw_triangle();T(copies==0&&stub.n_texture_create==1,"other address unchanged guest upload");down();
 up();m(M_CONTEXT_DMA_COLOR_A,DMA_MAIN);draw_triangle();T(copies==0&&stub.n_texture_create==1,"other location unchanged guest upload");down();
 up();m(M_TEX_UNIT+0x0C,0);draw_triangle();T(copies==0&&stub.n_draw==1,"disabled unit never snapshots");down();
 up();feedback_backend.color_snapshot=NULL;draw_triangle();T(copies==0&&stub.n_texture_create==1&&stub.n_draw==1,"absent callback preserves legacy fallback");down();
 up();fail_copy=1;draw_triangle();T(copies==1&&!stub.n_draw,"failed copy rejects and no emitted draw");down();
 up();fail_view=1;draw_triangle();T(copies==1&&!stub.n_draw,"failed view rejects and no emitted draw");down();
 up();draw_triangle();prior=snapshot_seen;m(M_SURFACE_CLIP_H,128<<16);m(M_COLOR_A_PITCH,512);m(M_TEX_UNIT+0x18,(128<<16)|256);m(0x1840,512);draw_triangle();T(copies==2&&!stub.alive[prior]&&stub.n_draw==2,"resize releases copied target before replacement");down();
 rejection(M_SURFACE_FORMAT,0x143);rejection(M_SURFACE_FORMAT,0x248);rejection(M_TEX_UNIT+0x18,(128<<16)|256);rejection(0x1840,512);rejection(0x1840,2048);rejection(M_TEX_UNIT+4,1|(2<<4)|(0x85<<8)|(1<<16));rejection(M_TEX_UNIT+4,1|(1<<4)|(0xA5<<8)|(1<<16));rejection(M_TEX_UNIT+4,1|(2<<4)|(0xA5<<8)|(2<<16));rejection(M_TEX_UNIT+4,1|4|(2<<4)|(0xA5<<8)|(1<<16));rejection(M_TEX_UNIT+4,1|(2<<4)|(0xAC<<8)|(1<<16));rejection(0x1840,1024|(2<<20));
 /* Direct production binding: two referenced units share one per-draw copy. */
 up();u32 targets[4];u32 nt=eng_current_target_set(targets);for(u32 a=0;a<0x20;a+=4)m(M_TEX_UNIT+0x20+a,rsx_dsp_reg(&g.rsx,M_TEX_UNIT+a));m(0x1844,1024);u32 tx[16]={0};rsx_be_sampler_desc ss[16]={0};T(sink_bind_textures(targets,nt,ENG_INVALID,3,tx,ss)==3&&copies==1&&tx[0]==tx[1],"two units reuse one immutable copy in a draw");memset(tx,0,sizeof tx);T(sink_bind_textures(targets,nt,ENG_INVALID,0,tx,ss)==0&&copies==1,"unused enabled units make no copy");down();
 /* Each owned MRT gets its own immutable image. */
 up();m(M_COLOR_TARGET,CELL_GCM_SURFACE_TARGET_MRT1);m(M_CONTEXT_DMA_COLOR_B,DMA_LOCAL);m(M_COLOR_B_OFFSET,0x50000);m(0x021C,1024);for(u32 a=0;a<0x20;a+=4)m(M_TEX_UNIT+0x20+a,rsx_dsp_reg(&g.rsx,M_TEX_UNIT+a));m(0x1844,1024);m(M_TEX_UNIT+0x20,0x50000);nt=eng_current_target_set(targets);memset(tx,0,sizeof tx);T(nt==2&&sink_bind_textures(targets,nt,ENG_INVALID,3,tx,ss)==3&&copies==2&&tx[0]!=tx[1],"MRT feedback snapshots every owned member independently");down();
 up();feedback_backend.surface_view=NULL;draw_triangle();T(copies==1&&stub.n_draw==1&&stub.bound_tex[0]==snapshot_seen,"absent view allows identity copy");down();
 up();feedback_backend.surface_view=NULL;m(M_TEX_UNIT+0x10,0xAA6C);draw_triangle();T(copies==1&&!stub.n_draw,"absent remap view rejects instead of silently changing channels");down();
 /* A compressed texture over the current target is memory, not feedback. */
 up();m(M_TEX_UNIT+4,1|(2<<4)|(0xA6<<8)|(1<<16));m(M_TEX_UNIT+0x18,(16<<16)|16);draw_triangle();T(copies==0&&stub.n_draw==1,"DXT1 at the target's offset uploads from memory instead of dropping the draw");down();
 /* Independently newer depth at the same address keeps its established route. */
 up();nt=eng_current_target_set(targets);u32 zs=eng_zdepth_get(RSX_LOCATION_LOCAL,0x40000,256,256);g.zdepths[zs].depth_serial=2;g.zdepths[zs].had_write=1;g.zdepths[zs].snapshot_valid=0;m(M_TEX_UNIT+4,1|(2<<4)|(0xB0<<8)|(1<<16));memset(tx,0,sizeof tx);T(sink_bind_textures(targets,nt,ENG_INVALID,1,tx,ss)==1&&copies==0&&stub.n_depth_snapshot==1,"newer depth wins over own color metadata");down();
 /* The zeta is canvas-sized; the snapshot covers only the texture's region. */
 up();nt=eng_current_target_set(targets);zs=eng_zdepth_get(RSX_LOCATION_LOCAL,0x40000,256,256);g.zdepths[zs].depth_serial=2;g.zdepths[zs].had_write=1;g.zdepths[zs].snapshot_valid=0;m(M_TEX_UNIT+4,1|(2<<4)|(0xB0<<8)|(1<<16));m(M_TEX_UNIT+0x18,(128<<16)|160);memset(tx,0,sizeof tx);T(g.zdepths[zs].w>128&&sink_bind_textures(targets,nt,ENG_INVALID,1,tx,ss)==1&&stub.snapshot_w==128&&stub.snapshot_h==160,"depth snapshot cropped to the sampled texture");down();
 /* Full physical layouts and matching formats, without reinterpretation. */
 const rsx_be_format f[]={RSX_BE_FMT_R8G8B8A8,RSX_BE_FMT_R16G16B16A16F,RSX_BE_FMT_R32G32B32A32F,RSX_BE_FMT_R32F};const u32 b[]={0xA5,0xBA,0xBB,0xBC},bp[]={4,8,16,4};for(unsigned i=0;i<4;i++){const u32 cf[4]={8,11,12,13};rsx_dsp_surface sf={.color_format=cf[i],.raster_type=1};eng_surface s={.w=128,.h=64,.fmt=f[i],.pitch=128*bp[i]};rsx_dsp_texture t={.format=b[i],.dimension=2,.width=128,.height=64,.mipmaps=1,.pitch=128*bp[i],.depth=1};T(eng_color_feedback_compatible(&s,&t,&sf),"compatible integer/float full physical layout");t.width=64;T(!eng_color_feedback_compatible(&s,&t,&sf),"logical width must not replace AA physical width");t.width=128;s.pitch=t.pitch=0;T(eng_color_feedback_compatible(&s,&t,&sf),"zero pitch means matching packed rows");s.pitch=t.pitch=128*bp[i]+64;T(eng_color_feedback_compatible(&s,&t,&sf),"matching padded pitch supported");}
 free(vm_base);printf("engine feedback checks=%u failures=%u\n",checks,failures);return failures?1:0;}

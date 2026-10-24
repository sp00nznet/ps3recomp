/* Synthetic real-engine same-target sampling. No title data.
 * Explicit AA0 full extent: sample-resolution AA is a separate feature. */
#include "rsx_metal_backend.m"
#define eng_texture_upload test_engine_texture_upload
#define eng_present test_engine_present
#include "rsx_draw_engine.c"
#include "rsx_tiling.c"
#undef eng_texture_upload
#undef eng_present
#include "rsx_test_programs.h"
#include <math.h>
u8* vm_base;u32 ppu_vm_size;
u32 cellGcmResolveLocated(int local,u32 offset){(void)local;return offset;}
u32 cellGcmResolveIO(u32 offset){return offset<ppu_vm_size?offset:0;}
void cellGcm_replay_display_buffers_to_engine(void){}
static unsigned checks,failures;
#define T(x,label) do{checks++;if(!(x)){failures++;if(failures<20)fprintf(stderr,"FAIL %s\n",label);}}while(0)
static void method(u32 a,u32 v){rsx_draw_engine_method(a,v);}
static void bf(u8*p,float f){u32 v;memcpy(&v,&f,4);p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void draw(void){method(0x1808,RSX_PRIMITIVE_TRIANGLES);method(0x1814,2u<<24);method(0x1808,0);}
static void run(u32 aa,u32 sx,u32 sy){
 T(rsx_draw_engine_init(64,64)==0,"initialization");
 method(0x194,0xFEED0000);method(0x210,0x40000);method(0x20C,64*sx*4);method(0x200,64u<<16);method(0x204,64u<<16);method(0x208,0x148|(aa<<12));
 method(0x1740,2u|(4u<<4)|(48u<<8));method(0x1680,0x80010000);method(0x1744,2u|(4u<<4)|(48u<<8));method(0x1684,0x80010010);method(0x1760,2u|(4u<<4)|(48u<<8));method(0x16A0,0x80010020);
 const float v[36]={-1,-1,.2,1,.25,.5,.75,.125,.25,.5,0,1,3,-1,.2,1,.25,.5,.75,.125,.25,.5,0,1,-1,3,.2,1,.25,.5,.75,.125,.25,.5,0,1};for(u32 i=0;i<36;i++)bf(vm_base+0x10000+i*4,v[i]);
 u8 vp[48];rsx_test_vp_mov_out(vp,0,RSX_TEST_VP_SWZ_IDENT,0,0);rsx_test_vp_mov_out(vp+16,1,RSX_TEST_VP_SWZ_IDENT,1,0);rsx_test_vp_mov_out(vp+32,8,RSX_TEST_VP_SWZ_IDENT,7,1);method(0x1E9C,0);for(u32 i=0;i<12;i++){u32 w;memcpy(&w,vp+i*4,4);method(0xB80+i*4,w);}method(0x1EA0,0);
 rsx_test_fp_mov_r0_col0(vm_base+0x60000,RSX_TEST_FP_SWZ_IDENT);method(0x8E4,0x60002);method(0x1D60,64);
 method(0xA6C,0x203);method(0xA70,1);method(0xA74,1);method(0x1D90,0xCC333366);method(0x1D8C,0xFFFFFF07);method(0x1D94,0xF3);draw();
 /* Producer remains queued: the snapshot must see its GPU output, not this clear. */
 T(s_eng_rec_count>=2,"producer remains queued before consumers");
 rsx_test_fp_tex_r0_tc0(vm_base+0x60040,0);method(0x8E4,0x60042);
 method(0x1A00,0x40000);method(0x1A04,1|(2<<4)|(0xA5<<8)|(1<<16));method(0x1A0C,0x80000000);method(0x1A10,0xAA6C);method(0x1A18,((64*sx)<<16)|(64*sy));method(0x1840,64*sx*4);
 method(0xA6C,0x202);method(0xA70,0);method(0x328,1);method(0x330,0x202);method(0x334,7);method(0x338,255);method(0x33C,0x1E00);method(0x340,0x1E00);method(0x344,0x1E00);
 method(0x8C0,32u<<16);method(0x8C4,64u<<16);draw();
 /* Second copy must include the first consumer's writes; its fixed UV lies left. */
 method(0x1A10,0xAAE4);method(0x8C0,(32u<<16)|32);draw();
 /* Long queued chain: replacing a copy also retires its view, but neither
  * numeric handle may disappear until prior records are encoded. */
#ifdef RSX_BE_HAS_COLOR_SNAPSHOT
 if (aa==0) {
  const u32 old_copy=g.surfaces[0].feedback;
  const u32 old_view=s_eng_pending.tex[0];
  for(u32 q=0;q<256;q++)draw();
  T(s_eng_obj_retired[old_copy-1]&&eng_obj(old_copy)!=nil,"released queued source retains texture until encoding");
  T(s_eng_obj_retired[old_view-1]&&eng_obj(old_view)!=nil,"released queued view retains texture until encoding");
 }
#endif
 s_engine_backend.submit_and_wait(NULL,0);T(s_eng_rec_count==0,"headless GPU submission finished encoding");
 u8 pixels[128*128*4];float z[128*128];s_engine_backend.readback(NULL,g.surfaces[0].handle,0,0,64*sx,64*sy,pixels,64*sx*4);
 const u32 ds=eng_depth_snapshot(NULL,g.zdepths[0].handle,64*sx,64*sy);T(ds!=0,"existing depth snapshot");s_engine_backend.submit_and_wait(NULL,0);[eng_obj(ds) getBytes:z bytesPerRow:64*sx*4 fromRegion:MTLRegionMake2D(0,0,64*sx,64*sy) mipmapLevel:0];eng_obj_release(NULL,ds);
 for(u32 i=0;i<64*sx*64*sy;i++){T(pixels[4*i]==191&&pixels[4*i+1]==128&&pixels[4*i+2]==64&&pixels[4*i+3]==32,"ordered self sampling preserves producer and prior consumer RGBA");T(fabsf(z[i]-.2f)<1e-6f,"encoder restart preserves depth");}
 T(s_eng_dropped==0,"no dropped records");
#ifdef RSX_BE_HAS_COLOR_SNAPSHOT
 if(aa==0){const u32 old_count=s_eng_obj_count;s_eng_rec_count=ENG_MAX_RECORDS;
  T(!eng_color_snapshot(NULL,g.surfaces[0].handle),"full record queue rejects snapshot");
  T(s_eng_obj_count==old_count,"queue rejection allocates no texture");
  T(s_eng_dropped>0,"queue rejection records explicit backend failure");
  s_eng_rec_count=0;}
#endif
 rsx_draw_engine_shutdown();
}

#ifdef RSX_BE_HAS_COLOR_SNAPSHOT
static void formats(void){
 const rsx_be_format ff[4]={RSX_BE_FMT_R8G8B8A8,RSX_BE_FMT_R16G16B16A16F,RSX_BE_FMT_R32G32B32A32F,RSX_BE_FMT_R32F};
 const float c[4]={.125f,.5f,1.5f,.75f};
 for(u32 f=0;f<4;f++){
  T(!eng_init(NULL,8,4),"format fixture initialization");
  u32 source=eng_color_target_create(NULL,ff[f],8,4,NULL,0);
  eng_clear_color(NULL,source,c);u32 copy=eng_color_snapshot(NULL,source);
  u32 dest=eng_color_target_create(NULL,RSX_BE_FMT_R32G32B32A32F,8,4,NULL,0);
  T(source&&copy&&dest&&source!=copy,"format copy has distinct storage");
  id<MTLCommandBuffer> cb=[s_queue commandBuffer];eng_encode_records(cb,nil);
  eng_blit_to_display(cb,eng_obj(copy),eng_obj(dest));T(s_eng_blit_pso!=nil,"sample copied storage into float target");
  [cb commit];[cb waitUntilCompleted];T([cb status]==MTLCommandBufferStatusCompleted&&![cb error],"format GPU completion");
  float pixels[8*4*4];[eng_obj(dest) getBytes:pixels bytesPerRow:8*16 fromRegion:MTLRegionMake2D(0,0,8,4) mipmapLevel:0];
  const float expected[4]={f==0?32.f/255:.125f,f==3?0:f==0?128.f/255:.5f,f==3?0:f==0?1:1.5f,f==3?1:f==0?191.f/255:.75f};
  for(u32 i=0;i<8*4;i++)for(u32 lane=0;lane<4;lane++)T(fabsf(pixels[i*4+lane]-expected[lane])<1e-6f,"copy preserves format/float magnitude/alpha");
  s_eng_rec_count=0;eng_shutdown(NULL);
 }
}
#endif

int main(void){@autoreleasepool{s_dev=MTLCreateSystemDefaultDevice();if(!s_dev)return 2;s_queue=[s_dev newCommandQueue];s_headless=1;s_guest_shaders=1;vm_base=calloc(1,1u<<20);ppu_vm_size=1u<<20;rsx_draw_engine_set_backend(&s_engine_backend);run(0,1,1);
#ifdef RSX_BE_HAS_COLOR_SNAPSHOT
formats();
#endif
rsx_draw_engine_set_backend(NULL);free(vm_base);printf("GPU feedback checks=%u failures=%u\n",checks,failures);return failures?1:0;}}

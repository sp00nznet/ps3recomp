/* Synthetic color-input boundary, no original programs or assets. */
#include "rsx_metal_backend.m"
#include "rsx_test_programs.h"
#include <math.h>
u8* vm_base;u32 cellGcmResolveLocated(int local,u32 off){(void)local;return off;}
static int checks,failures;
#define CHECK(c,n) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",n);}}while(0)
int main(void){@autoreleasepool{s_dev=MTLCreateSystemDefaultDevice();if(!s_dev)return 2;s_queue=[s_dev newCommandQueue];vm_base=calloc(1,4096);
 const char*src="#include <metal_stdlib>\nusing namespace metal;struct V{float4 p[[position]];float4 c0[[user(locn0)]];float4 c1[[user(locn1)]];float4 f[[user(locn2)]];float4 t[[user(locn3)]];float4 t9[[user(locn12)]];};vertex V vs(uint i[[vertex_id]],constant float4& value[[buffer(0)]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return{float4(p[i],0,1),value,value,value,value,value};}";NSError*error=nil;id<MTLLibrary>vl=[s_dev newLibraryWithSource:[NSString stringWithUTF8String:src] options:nil error:&error];CHECK(vl!=nil,"vertex library");if(!vl)return 2;
 MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;id<MTLTexture>out=[s_dev newTextureWithDescriptor:td];rsx_state st;memset(&st,0,sizeof st);st.shader_program=257;st.shader_control=64;const u8*uc=NULL;
 const float values[4][4]={{-2,.25f,1.25f,-128},{0,.75f,1,2},{2,2,2,2},{-.25f,-.5f,-.75f,-1}};
#ifdef RSX_COLOR_TEST_TEX9
 const unsigned attrs[]={1,2,3,4,13}; /* Native baseline also supports TC9. */
#else
 const unsigned attrs[]={1,2,3,4};
#endif
 const unsigned ops[3]={1,2,4};
 for(int width=0;width<2;width++)for(int opindex=0;opindex<3;opindex++)for(unsigned ai=0;ai<sizeof(attrs)/sizeof(attrs[0]);ai++)for(int modifier=0;modifier<3;modifier++)for(int swizzle=0;swizzle<2;swizzle++)for(int vi=0;vi<4;vi++){
  st.shader_control=width?64:0;
  u32 swz=swizzle?RSX_TEST_FP_SWZ(3,2,1,0):RSX_TEST_FP_SWZ_IDENT;u32 src0=RSX_TEST_FP_SRC_INPUT|swz;if(modifier)src0|=1u<<17;if(modifier==2)src0|=1u<<29;
  rsx_test_fp_instr(vm_base+256,RSX_TEST_FP_OPC(ops[opindex])|RSX_TEST_FP_MASK_XYZW|(attrs[ai]<<13)|(width?0:1u<<7)|RSX_TEST_FP_END,src0);
  rsx_test_fp_put_word(vm_base+264,RSX_TEST_FP_SRC_INPUT|swz);rsx_test_fp_put_word(vm_base+268,RSX_TEST_FP_SRC_INPUT|swz);int slot=fp_slot_for(&st,&uc,0);CHECK(slot>=0,"FP color source compilation");if(slot<0)continue;
  MTLRenderPipelineDescriptor*pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[vl newFunctionWithName:@"vs"];pd.fragmentFunction=s_fp_cache[slot].fn;pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Float;id<MTLRenderPipelineState>p=[s_dev newRenderPipelineStateWithDescriptor:pd error:&error];CHECK(p!=nil,"actual translated pipeline");if(!p)return 2;
  MTLRenderPassDescriptor*rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=out;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;id<MTLCommandBuffer>cb=[s_queue commandBuffer];id<MTLRenderCommandEncoder>e=[cb renderCommandEncoderWithDescriptor:rp];[e setRenderPipelineState:p];[e setVertexBytes:values[vi] length:16 atIndex:0];[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];[cb commit];[cb waitUntilCompleted];CHECK(cb.status==MTLCommandBufferStatusCompleted&&!cb.error,"GPU completes");float pixel[4];[out getBytes:pixel bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];int good=1;for(int lane=0;lane<4;lane++){float base=values[vi][swizzle?3-lane:lane];if(ai<2)base=fminf(1,fmaxf(0,base));float want=base;if(modifier==2)want=fabsf(want);if(modifier)want=-want;if(opindex)want=want*base;if(opindex==2)want+=base;good &=fabsf(pixel[lane]-want)<1e-6;}CHECK(good,"COL0/COL1 clamp precedes modifiers; TC remains unrestricted");if(!good)fprintf(stderr,"DETAIL attr%u modifier%d input%d output%g,%g,%g,%g\n",attrs[ai],modifier,vi,pixel[0],pixel[1],pixel[2],pixel[3]);
 }
 printf("Color input boundary: %d checks, %d failures\n",checks,failures);free(vm_base);return failures?1:0;}}

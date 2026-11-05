/* Synthetic shadow comparison probe; no title programs, assets or target process. */
#include "rsx_metal_backend.m"
#include "rsx_test_programs.h"
#include <stdio.h>
#include <math.h>
u8* vm_base;
u32 cellGcmResolveLocated(int local,u32 off){(void)local;return off;}
static int checks,failures;
#define CHECK(c,n) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",n);}}while(0)
static float expected(unsigned mode,float ref,float d){switch(mode){case 1:return d<ref;case 2:return d==ref;case 3:return d<=ref;case 4:return d>ref;case 5:return d!=ref;case 6:return d>=ref;default:return d;}}
int main(void){@autoreleasepool{
 s_dev=MTLCreateSystemDefaultDevice();if(!s_dev)return 2;s_queue=[s_dev newCommandQueue];vm_base=calloc(1,4096);
 rsx_test_fp_instr(vm_base+256,RSX_TEST_FP_OPC(0x18)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_INSRC_TC0|RSX_TEST_FP_END,RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);
 const char* vsrc="#include <metal_stdlib>\nusing namespace metal;struct V {float4 p [[position]];float4 t [[user(locn3)]];};vertex V vs(uint i [[vertex_id]],constant float4& tc [[buffer(0)]]) {float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return {float4(p[i],0,1),tc};}";
 NSError*err=nil;id<MTLLibrary>vl=[s_dev newLibraryWithSource:[NSString stringWithUTF8String:vsrc] options:nil error:&err];CHECK(vl!=nil,"vertex library");if(!vl)return 2;
 MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:1 height:1 mipmapped:NO];td.usage=MTLTextureUsageShaderRead;id<MTLTexture>depth=[s_dev newTextureWithDescriptor:td];float d=.5f;[depth replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&d bytesPerRow:4];
 td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;id<MTLTexture>out=[s_dev newTextureWithDescriptor:td];MTLSamplerDescriptor*sd=[MTLSamplerDescriptor new];sd.minFilter=MTLSamplerMinMagFilterNearest;sd.magFilter=MTLSamplerMinMagFilterNearest;sd.sAddressMode=sd.tAddressMode=MTLSamplerAddressModeClampToEdge;id<MTLSamplerState>sampler=[s_dev newSamplerStateWithDescriptor:sd];
 rsx_state st;memset(&st,0,sizeof st);st.shader_program=257;st.shader_control=64;st.textures[0].format=(0xB0u<<8)|(1u<<16)|(2u<<4)|1u;st.textures[0].control0=0x80000000;st.textures[0].filter=0x01010000;st.textures[0].image_rect=0x10001;
 const u8*uc=NULL;
 for(unsigned mode=0;mode<8;mode++)for(int refindex=0;refindex<4;refindex++){
  float tc[4]={refindex==3?1.0f:.5f,refindex==3?1.0f:.5f,refindex==0?.25f:refindex==1?.5f:refindex==2?.75f:1.5f,refindex==3?2.0f:1.0f};st.textures[0].address=(mode<<28)|0x30303;
  int slot=fp_slot_for(&st,&uc,0);CHECK(slot>=0,"guest TXP compilation");if(slot<0)continue;MTLRenderPipelineDescriptor*pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[vl newFunctionWithName:@"vs"];pd.fragmentFunction=s_fp_cache[slot].fn;pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Float;id<MTLRenderPipelineState>pipe=[s_dev newRenderPipelineStateWithDescriptor:pd error:&err];CHECK(pipe!=nil,"actual translated fragment pipeline");if(!pipe)return 2;
  MTLRenderPassDescriptor*rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=out;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;id<MTLCommandBuffer>cb=[s_queue commandBuffer];id<MTLRenderCommandEncoder>e=[cb renderCommandEncoderWithDescriptor:rp];[e setRenderPipelineState:pipe];[e setVertexBytes:tc length:sizeof(tc) atIndex:0];[e setFragmentTexture:depth atIndex:0];[e setFragmentSamplerState:sampler atIndex:0];[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];[cb commit];[cb waitUntilCompleted];CHECK(cb.status==MTLCommandBufferStatusCompleted&&!cb.error,"GPU draw completes");float pixel[4]={0};[out getBytes:pixel bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];float want=expected(mode,tc[2]/tc[3],d);CHECK(fabsf(pixel[0]-want)<1e-6,"stored-depth-to-reference compare or raw endpoint bypass");printf("mode%u reference%g stored%g actual%g expected%g\n",mode,tc[2]/tc[3],d,pixel[0],want);
 }
 MtlDraw draw;memset(&draw,0,sizeof draw);
 st.textures[0].address=(3u<<28)|0x30303;
 CHECK(guest_programs_for(&st,&draw)==-1,"active comparison cannot fall back on a missing guest VP");
 st.textures[0].format=(0xB0u<<8)|(2u<<16)|(2u<<4)|1u;
 CHECK(fp_slot_for(&st,&uc,0)==-2,"unsupported mip layout rejects legacy shader");
 CHECK(fp_slot_for(&st,&uc,0)==-2,"cached unsupported layout still rejects legacy shader");
 CHECK(guest_programs_for(&st,&draw)==-1,"unsupported comparison rejects legacy draw");
 printf("Projected depth compare: %d checks, %d failures\n",checks,failures);free(vm_base);return failures?1:0;
}}

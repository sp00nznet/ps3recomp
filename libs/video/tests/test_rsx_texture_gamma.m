/* Synthetic post-filter/post-remap gamma check. No title data. */
#include "../rsx_metal_backend.m"
u8* vm_base;
u32 cellGcmResolveLocated(int local,u32 off){(void)local;return off;}
static unsigned checks,failures;
#define CHECK(x,label) do{++checks;if(!(x)){++failures;fprintf(stderr,"FAIL %s\n",label);}}while(0)
static float decode(float v){return v>.04045f?powf((v+.055f)/1.055f,2.4f):v/12.92f;}
int main(int argc,char** argv)
{
 if(argc!=2)return 2;
 @autoreleasepool {
 s_dev=MTLCreateSystemDefaultDevice();if(!s_dev)return 2;s_queue=[s_dev newCommandQueue];NSError* error=nil;
 const char* vs="#include <metal_stdlib>\nusing namespace metal;struct V{float4 p[[position]];float4 t[[user(locn3)]];};vertex V vs(uint i[[vertex_id]],constant float4& t[[buffer(0)]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return{float4(p[i],0,1),t};}";
 id<MTLLibrary> vl=[s_dev newLibraryWithSource:[NSString stringWithUTF8String:vs]options:nil error:&error];CHECK(vl!=nil,"synthetic TC0 vertex shader");if(!vl)return 2;
 MTLTextureDescriptor* td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
 id<MTLTexture> texture=[s_dev newTextureWithDescriptor:td];const u8 bytes[16]={0,15,64,80,64,55,128,180,128,180,250,200,255,240,160,250};[texture replaceRegion:MTLRegionMake2D(0,0,2,2)mipmapLevel:0 withBytes:bytes bytesPerRow:8];
 MTLTextureSwizzleChannels swap={MTLTextureSwizzleBlue,MTLTextureSwizzleAlpha,MTLTextureSwizzleRed,MTLTextureSwizzleGreen};
 id<MTLTexture> remapped=[texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:MTLTextureType2D levels:NSMakeRange(0,1)slices:NSMakeRange(0,1)swizzle:swap];CHECK(remapped!=nil,"asymmetric resource crossbar view");if(!remapped)return 2;
 td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;id<MTLTexture> output=[s_dev newTextureWithDescriptor:td];
 MTLSamplerDescriptor* sd=[MTLSamplerDescriptor new];sd.minFilter=MTLSamplerMinMagFilterLinear;sd.magFilter=MTLSamplerMinMagFilterLinear;sd.sAddressMode=MTLSamplerAddressModeClampToEdge;sd.tAddressMode=MTLSamplerAddressModeClampToEdge;id<MTLSamplerState> sampler=[s_dev newSamplerStateWithDescriptor:sd];
 const unsigned lanes[2][4]={{0,1,2,3},{2,3,0,1}};
 for(unsigned unitIndex=0;unitIndex<2;++unitIndex)for(unsigned op=0;op<4;++op)for(unsigned mask=0;mask<16;++mask){const unsigned unit=unitIndex?15:0;char path[1024];snprintf(path,sizeof path,"%s/u%u-op%u-mask%u.hlsl",argv[1],unit,op,mask);FILE* f=fopen(path,"rb");if(!f)return 2;fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);char* source=calloc(1,(size_t)size+1);fread(source,1,(size_t)size,f);fclose(f);id<MTLFunction> fp=eng_function(source,RSX_SHADER_STAGE_FRAGMENT,"synthetic gamma");free(source);CHECK(fp!=nil,"actual translated sample shader compiles");if(!fp)return 2;
 MTLRenderPipelineDescriptor* pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[vl newFunctionWithName:@"vs"];pd.fragmentFunction=fp;pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Float;id<MTLRenderPipelineState> pipeline=[s_dev newRenderPipelineStateWithDescriptor:pd error:&error];CHECK(pipeline!=nil,"gamma observation pipeline");if(!pipeline)return 2;
 for(unsigned view=0;view<2;++view)for(unsigned point=0;point<3;++point){const float uv[3][2]={{.375f,.625f},{.5f,.5f},{.25f,.25f}};float tc[4]={uv[point][0],uv[point][1],0,1};if(op==1){tc[0]*=2;tc[1]*=2;tc[3]=2;}
 MTLRenderPassDescriptor* rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=output;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;id<MTLCommandBuffer> cb=[s_queue commandBuffer];id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:rp];[e setRenderPipelineState:pipeline];[e setVertexBytes:tc length:16 atIndex:0];[e setFragmentTexture:view?remapped:texture atIndex:unit];[e setFragmentSamplerState:sampler atIndex:unit];[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];[cb commit];[cb waitUntilCompleted];CHECK(cb.status==MTLCommandBufferStatusCompleted&&!cb.error,"GPU completes");float got[4];[output getBytes:got bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,1,1)mipmapLevel:0];const float x=uv[point][0]*2-.5f,y=uv[point][1]*2-.5f;const float weights[4]={(1-x)*(1-y),x*(1-y),(1-x)*y,x*y};for(unsigned c=0;c<4;++c){float sampled=0;for(unsigned p=0;p<4;++p)sampled+=(float)bytes[p*4+lanes[view][c]]/255.f*weights[p];const float expected=(mask&(1u<<c))?decode(sampled):sampled;CHECK(fabsf(got[c]-expected)<2e-5f,"post-filter final-lane gamma and ordinary channels");}}
 }
 printf("texture gamma GPU: %u checks/%u failures\n",checks,failures);return failures?1:0;
 }
}

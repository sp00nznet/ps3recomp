/* Synthetic GPU depth -> backend raw readback -> D24S8/A8R8G8B8 alias ->
 * GPU position-depth reconstruction. No window or original game data.
 * clang -fobjc-arc -I include -I libs/video
 * libs/video/tests/test_metal_depth_readback.m libs/video/rsx_texture_layout.c libs/video/rsx_shader_msl.cpp -lc++
 * -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore
 * -o /tmp/test_metal_depth_readback */
#include "../rsx_metal_backend.m"
#include <math.h>
#include <stdio.h>
static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr,"FAIL %s\n",name); } } while (0)
static const char* fixture =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct V { float4 p [[position]]; };\n"
"vertex V vs(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return {float4(p[i],0,1)}; }\n"
"struct D { float d [[depth(any)]]; }; fragment D fs(V v [[stage_in]]) { return {0.125f + floor(v.p.x)*0.125f + floor(v.p.y)*0.0625f}; }\n"
"fragment float alias_fs(V v [[stage_in]], texture2d<float> t [[texture(0)]]) { float3 rgb=t.read(uint2(v.p.xy)).rgb; return dot(rgb,float3(16711680.0f,65280.0f,255.0f))/16777215.0f; }\n";
static int complete(id<MTLCommandBuffer> cb) { [cb commit]; [cb waitUntilCompleted]; return cb.status==MTLCommandBufferStatusCompleted && !cb.error; }
static id<MTLRenderPipelineState> pipeline(id<MTLLibrary> lib, const char* fn, int depth)
{
    MTLRenderPipelineDescriptor* p=[MTLRenderPipelineDescriptor new];
    p.vertexFunction=[lib newFunctionWithName:@"vs"];
    p.fragmentFunction=[lib newFunctionWithName:[NSString stringWithUTF8String:fn]];
    if (depth) { p.depthAttachmentPixelFormat=MTL_DEPTH_FORMAT; p.stencilAttachmentPixelFormat=MTL_DEPTH_FORMAT; }
    else p.colorAttachments[0].pixelFormat=MTLPixelFormatR32Float;
    NSError* error=nil; id<MTLRenderPipelineState> r=[s_dev newRenderPipelineStateWithDescriptor:p error:&error];
    CHECK(r!=nil,"synthetic pipeline"); return r;
}
static int write_depth(u32 handle,id<MTLRenderPipelineState> p)
{
    MTLRenderPassDescriptor* rp=[MTLRenderPassDescriptor renderPassDescriptor];
    rp.depthAttachment.texture=eng_obj(handle); rp.depthAttachment.loadAction=MTLLoadActionClear; rp.depthAttachment.clearDepth=1; rp.depthAttachment.storeAction=MTLStoreActionStore;
    rp.stencilAttachment.texture=eng_obj(handle); rp.stencilAttachment.loadAction=MTLLoadActionClear; rp.stencilAttachment.storeAction=MTLStoreActionStore;
    id<MTLCommandBuffer> cb=[s_queue commandBuffer]; id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:rp];
    MTLDepthStencilDescriptor* ds=[MTLDepthStencilDescriptor new]; ds.depthCompareFunction=MTLCompareFunctionAlways; ds.depthWriteEnabled=YES;
    [e setRenderPipelineState:p]; [e setDepthStencilState:[s_dev newDepthStencilStateWithDescriptor:ds]];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding]; return complete(cb);
}
static int read_depth(u32 handle,u32 x,u32 y,u32 w,u32 h,float* out,u32 pitch)
{ return s_engine_backend.depth_readback ? s_engine_backend.depth_readback(NULL,handle,x,y,w,h,out,pitch) : -1; }
static float expected(u32 x,u32 y) { return .125f+x*.125f+y*.0625f; }
int main(void)
{
    @autoreleasepool {
        s_dev=MTLCreateSystemDefaultDevice(); CHECK(s_dev!=nil,"Metal device"); if (!s_dev) return 1;
        s_queue=[s_dev newCommandQueue]; CHECK(eng_init(NULL,4,3)==0,"backend helpers");
        NSError* error=nil; id<MTLLibrary> lib=[s_dev newLibraryWithSource:[NSString stringWithUTF8String:fixture] options:nil error:&error];
        CHECK(lib!=nil,"synthetic shaders"); if (!lib) { fprintf(stderr,"%s\n",error.localizedDescription.UTF8String); return 1; }
        u32 depth=eng_depth_target_create(NULL,4,3); CHECK(depth!=0,"depth target");
        CHECK(write_depth(depth,pipeline(lib,"fs",1)),"nonuniform GPU depth write");
        /* Preserve the preexisting depth-snapshot path independently. */
        u32 snap=eng_depth_snapshot(NULL,depth,4,3); eng_submit_and_wait(NULL,0);
        float baseline[12]={0}; [eng_obj(snap) getBytes:baseline bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,3) mipmapLevel:0];
        int baseline_ok=1; for (u32 y=0;y<3;y++) for(u32 x=0;x<4;x++) baseline_ok &= fabsf(baseline[y*4+x]-expected(x,y))<1e-6;
        CHECK(baseline_ok,"existing depth snapshot preserves nonuniform source"); eng_obj_release(NULL,snap);
        /* A smaller snapshot is the top-left region 1:1, not the whole target
         * scaled down: a texture smaller than its canvas-sized zeta. */
        u32 crop=eng_depth_snapshot(NULL,depth,2,2); eng_submit_and_wait(NULL,0);
        CHECK(crop && [eng_obj(crop) width]==2 && [eng_obj(crop) height]==2,"cropped snapshot size");
        float cropped[4]={0}; if (crop) [eng_obj(crop) getBytes:cropped bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
        CHECK(fabsf(cropped[0]-expected(0,0))<1e-6 && fabsf(cropped[1]-expected(1,0))<1e-6 &&
              fabsf(cropped[2]-expected(0,1))<1e-6 && fabsf(cropped[3]-expected(1,1))<1e-6,"cropped snapshot keeps top-left texels");
        if (crop) eng_obj_release(NULL,crop);
        CHECK(s_engine_backend.depth_readback!=NULL,"backend advertises depth readback");
        float out[18]; for(int i=0;i<18;i++)out[i]=-7;
        CHECK(read_depth(depth,0,0,4,3,out,24)==0,"full depth readback with padded rows");
        int ok=1;for(u32 y=0;y<3;y++)for(u32 x=0;x<6;x++)ok &= fabsf(out[y*6+x]-(x<4?expected(x,y):-7))<1e-6;
        CHECK(ok,"raw depth and output padding preserved");
        float roi[6]={-9,-9,-9,-9,-9,-9};
        CHECK(read_depth(depth,1,1,2,2,roi,12)==0,"offset ROI readback");
        CHECK(fabsf(roi[0]-expected(1,1))<1e-6 && fabsf(roi[1]-expected(2,1))<1e-6 && fabsf(roi[3]-expected(1,2))<1e-6 && fabsf(roi[4]-expected(2,2))<1e-6 && roi[2]==-9 && roi[5]==-9,"ROI uses full-source coordinates and keeps padding");
        u32 color=eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,4,3,NULL,0);
        const u32 count_before=s_eng_obj_count,free_before=s_eng_obj_free_count,rec_before=s_eng_rec_count;
        const struct {u32 handle,x,y,w,h,pitch; int null_out;} bad[]={
            {0,0,0,1,1,4,0},{color,0,0,1,1,4,0},{depth,0,0,0,1,4,0},
            {depth,4,0,1,1,4,0},{depth,0,3,1,1,4,0},{depth,UINT32_MAX,0,2,1,8,0},
            {depth,0,0,UINT32_MAX,1,UINT32_MAX,0},{depth,0,0,2,1,4,0},{depth,0,0,1,1,4,1}};
        for(u32 i=0;i<sizeof bad/sizeof bad[0];i++) {
            float guard=-17; CHECK(read_depth(bad[i].handle,bad[i].x,bad[i].y,bad[i].w,bad[i].h,bad[i].null_out?NULL:&guard,bad[i].pitch)!=0,"invalid request rejected"); CHECK(guard==-17,"failure preserves caller output");
        }
        CHECK(s_eng_obj_count==count_before && s_eng_obj_free_count==free_before && s_eng_rec_count==rec_before,"invalid requests do not allocate or queue work");
        /* Canonical D24S8 bytes, decoded through the actual texture decoder. */
        u8 packed[48],rgba[48];
        for(u32 y=0;y<3;y++)for(u32 x=0;x<4;x++) {double d=fmax(0.0,fmin(1.0,out[y*6+x]));u32 z=(u32)floor(d*16777215.0+.5);u8* b=packed+4*(y*4+x);b[0]=z>>16;b[1]=z>>8;b[2]=z;b[3]=0;}
        rsx_tex_layout tl; rsx_texture_layout(0xA5,4,3,&tl); rsx_texture_decode(rgba,16,packed,4,3,&tl,0);
        u32 alias=eng_texture_create(NULL,RSX_BE_FMT_R8G8B8A8,4,3,1,1,0xAA93,0xA5);
        eng_texture_upload(NULL,alias,0,0,4,3,rgba,16,0);
        MTLTextureDescriptor* td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:4 height:3 mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModeShared;
        id<MTLTexture> reconstructed=[s_dev newTextureWithDescriptor:td];MTLRenderPassDescriptor* rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=reconstructed;rp.colorAttachments[0].loadAction=MTLLoadActionDontCare;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
        id<MTLCommandBuffer> cb=[s_queue commandBuffer];id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:rp];[e setRenderPipelineState:pipeline(lib,"alias_fs",0)];[e setFragmentTexture:eng_obj(alias) atIndex:0];[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];CHECK(complete(cb),"GPU depth-byte alias reconstruction");
        float vals[12]={0};[reconstructed getBytes:vals bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,3) mipmapLevel:0];ok=1;for(u32 y=0;y<3;y++)for(u32 x=0;x<4;x++)ok &= fabsf(vals[y*4+x]-expected(x,y))<2e-6;CHECK(ok,"A5 and remap reconstruct original nonzero depths");
        eng_clear_depth_stencil(NULL,depth,3,1.0f,0);CHECK(read_depth(depth,0,0,4,3,out,24)==0,"pending depth clear drained before readback");ok=1;for(u32 y=0;y<3;y++)for(u32 x=0;x<4;x++)ok &= out[y*6+x]==1;CHECK(ok,"clear values visible at all source pixels");
        eng_submit_and_wait(NULL,0);
        eng_obj_release(NULL,alias);eng_obj_release(NULL,color);eng_obj_release(NULL,depth);eng_collect_retired_objects();ok=1;for(u32 i=0;i<s_eng_obj_count;i++)ok &= s_eng_obj[i]==nil;CHECK(ok,"temporary snapshot ownership leaves no live objects");
        printf("%d checks, %d failures\n",checks,failures);eng_shutdown(NULL);return failures?1:0;
    }
}

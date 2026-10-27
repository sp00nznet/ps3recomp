#include "../rsx_metal_backend.m"
#include <assert.h>
static id<MTLTexture> depth_texture(unsigned width,unsigned height) {
 MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:width height:height mipmapped:NO];
 d.usage=MTLTextureUsageRenderTarget;d.storageMode=MTLStorageModePrivate;
 id<MTLTexture> t=[s_dev newTextureWithDescriptor:d];assert(t);return t;
}
static void check_depth_stencil(id<MTLTexture> t,float depth,unsigned char stencil) {
 NSUInteger row=256,size=row*t.height;
 id<MTLBuffer> db=[s_dev newBufferWithLength:size options:MTLResourceStorageModeShared];
 id<MTLBuffer> sb=[s_dev newBufferWithLength:size options:MTLResourceStorageModeShared];assert(db && sb);
 id<MTLCommandBuffer> cb=[s_queue commandBuffer];id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];assert(blit);
 [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:db destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:size options:MTLBlitOptionDepthFromDepthStencil];
 [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:sb destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:size options:MTLBlitOptionStencilFromDepthStencil];
 [blit endEncoding];[cb commit];[cb waitUntilCompleted];assert(cb.status==MTLCommandBufferStatusCompleted);
 for(NSUInteger y=0;y<t.height;y++)for(NSUInteger x=0;x<t.width;x++) {
  float actual;memcpy(&actual,(unsigned char*)db.contents+y*row+x*4,4);assert(fabsf(actual-depth)<0.000001f);
  assert(*((unsigned char*)sb.contents+y*row+x)==stencil);
 }
}
int main(int argc,char**argv) {
 @autoreleasepool {
  assert(argc==2);s_dev=MTLCreateSystemDefaultDevice();assert(s_dev);
  s_queue=[s_dev newCommandQueue];assert(s_queue);
   /* Independent clears must cover each entire surface, even when consecutive
    * records name targets of different dimensions. They are not a draw MRT. */
   MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];d.usage=MTLTextureUsageRenderTarget;d.storageMode=MTLStorageModeShared;
   id<MTLTexture> a=[s_dev newTextureWithDescriptor:d];assert(a);
   d.width=4;d.height=3;
   id<MTLTexture> b=[s_dev newTextureWithDescriptor:d];assert(b);
   u32 ha=eng_obj_add(a),hb=eng_obj_add(b);
   float red[4]={1,0,0,1},green[4]={0,1,0,1};
   int with_depth=!strcmp(argv[1],"independent_depth_stencil");
   id<MTLTexture> da=nil,db=nil;u32 hda=0,hdb=0;
   if(with_depth) {da=depth_texture(5,4);db=depth_texture(7,3);hda=eng_obj_add(da);hdb=eng_obj_add(db);}
   eng_clear_color(NULL,ha,red);
   if(with_depth)eng_clear_depth_stencil(NULL,hda,RSX_BE_CLEAR_DEPTH|RSX_BE_CLEAR_STENCIL,0.25f,7);
   eng_clear_color(NULL,hb,green);
   if(with_depth) {
    eng_clear_depth_stencil(NULL,hdb,RSX_BE_CLEAR_DEPTH|RSX_BE_CLEAR_STENCIL,0.33f,13);
    eng_clear_depth_stencil(NULL,hda,RSX_BE_CLEAR_DEPTH,0.75f,9);
    eng_clear_depth_stencil(NULL,hda,RSX_BE_CLEAR_STENCIL,0.5f,11);
   }
   id<MTLCommandBuffer> cb=[s_queue commandBuffer];assert(cb);
   eng_encode_records(cb,nil);[cb commit];[cb waitUntilCompleted];
   assert(cb.status==MTLCommandBufferStatusCompleted);
   unsigned char pixels[48]={0};
   [a getBytes:pixels bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
   for(unsigned j=0;j<16;j+=4)assert(pixels[j]==255 && pixels[j+1]==0 && pixels[j+2]==0 && pixels[j+3]==255);
   [b getBytes:pixels bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,3) mipmapLevel:0];
   for(unsigned j=0;j<48;j+=4)assert(pixels[j]==0 && pixels[j+1]==255 && pixels[j+2]==0 && pixels[j+3]==255);
   if(with_depth) {check_depth_stencil(da,0.75f,11);check_depth_stencil(db,0.33f,13);}
  puts("Metal independent clears scenario passed");
 }
}

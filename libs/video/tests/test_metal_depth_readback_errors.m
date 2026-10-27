/* Run the nominal synthetic GPU fixture, then inject concrete renderer
 * encoding failures. Command buffers still complete: readback must reject
 * the skipped work without changing any caller bytes, including padding.
 * Build as test_metal_depth_readback.m, substituting this filename. */
#define main depth_nominal_main
#include "test_metal_depth_readback.m"
#undef main
#include <objc/runtime.h>
static IMP original_encoder;
static int fail_mode, injected;
static id fault_encoder(id self, SEL sel, MTLRenderPassDescriptor* rp)
{
    const int resolve = rp.colorAttachments[0].texture.pixelFormat == MTLPixelFormatR32Float;
    const int clear = rp.depthAttachment.texture && rp.depthAttachment.loadAction == MTLLoadActionClear;
    if ((fail_mode == 1 && resolve) || (fail_mode == 2 && clear)) { injected++; return nil; }
    return ((id(*)(id, SEL, MTLRenderPassDescriptor*))original_encoder)(self, sel, rp);
}
static void error_case(int kind)
{
    CHECK(eng_init(NULL,4,3)==0,"error-case backend initialization");
    NSError* error=nil;
    id<MTLLibrary> lib=[s_dev newLibraryWithSource:[NSString stringWithUTF8String:fixture] options:nil error:&error];
    CHECK(lib!=nil,"error-case shader");
    u32 source=eng_depth_target_create(NULL,4,3);
    CHECK(write_depth(source,pipeline(lib,"fs",1)),"error-case known GPU source");
    id<MTLCommandBuffer> probe=[s_queue commandBuffer];
    Method m=class_getInstanceMethod([probe class],@selector(renderCommandEncoderWithDescriptor:));
    injected=0; fail_mode=kind==1?1:2;
    if (kind<=3) original_encoder=method_setImplementation(m,(IMP)fault_encoder);
    if (kind==2 || kind==3) eng_clear_depth_stencil(NULL,source,3,1,0);
    u32 color=0;
    if (kind>=4) {
        /* A synthetic pending draw that the existing encoder must skip.
         * Its source depth is otherwise valid and its resolve can complete. */
        if (kind==4 || kind==7) color=eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,4,3,NULL,0);
        float dummy[4]={0};u32 offset=0;
        CHECK(eng_stage_copy(dummy,sizeof dummy,&offset),"pending-draw staging");
        EngRecord* r=&s_eng_rec[s_eng_rec_count++];memset(r,0,sizeof *r);
        r->kind=ENG_REC_DRAW;r->depth=kind==6?0:source;r->nrt=kind==6?0:1;r->rt[0]=color;
        if (kind==7) { s_eng_pipe_count=1;r->pipeline=1; } /* nil pipeline object */
    }
    /* The engine's depth-to-guest path submits before invoking readback.
     * A fresh snapshot must not erase that preceding encoding failure. */
    if (kind==3) {
        eng_submit_and_wait(NULL,0);
        method_setImplementation(m,original_encoder);
    }
    float guard[18];for(int i=0;i<18;i++)guard[i]=-17;
    int result=read_depth(source,0,0,4,3,guard,24);
    if (kind<=2) method_setImplementation(m,original_encoder);
    if (color) eng_obj_release(NULL,color);
    CHECK(injected==(kind<=3?1:0),"only intended encoder failure injected");
    CHECK(result!=0,"completed command buffer does not certify skipped work");
    int unchanged=1;for(int i=0;i<18;i++)unchanged &= guard[i]==-17;
    CHECK(unchanged,"encoding failure preserves entire output and padding");
    int live=0,retired=0;for(u32 i=0;i<s_eng_obj_count;i++){live+=s_eng_obj[i]!=nil;retired+=s_eng_obj_retired[i]!=0;}
    CHECK(live==1 && !retired && !s_eng_rec_count && eng_obj(source)!=nil,"failed readback retires only its temporary snapshot");
    printf("fault=%d result=%d output_unchanged=%d live=%d retired=%d pending=%u\n",kind,result,unchanged,live,retired,s_eng_rec_count);
    eng_obj_release(NULL,source);eng_shutdown(NULL);
}
int main(void)
{
    int result=depth_nominal_main();if(result)return result;
    @autoreleasepool { for(int kind=1;kind<=7;kind++)error_case(kind); }
    printf("%d total checks, %d failures\n",checks,failures);
    return failures?1:0;
}

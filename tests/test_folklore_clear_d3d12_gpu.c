/* Run the shared synthetic FIFO/pixel oracle through the real D3D12 backend. */
#include "../libs/video/rsx_d3d12_backend.c"
#include <assert.h>
const uint64_t function_table_count = 0;
const struct { uint64_t addr; void* func; const char* name; } function_table[1] = {{0,0,0}};
static u32 fixture_draws;
static int fixture_init(u32 width,u32 height,const char* title)
{
    int result=rsx_d3d12_backend_init(width,height,title);
    if(!result) ShowWindow(s_d3d.hwnd,SW_HIDE);
    return result;
}
static void fixture_present(void)
{
    fixture_draws=0;
    for(u32 i=0;i<s_d3d.draw_count;++i) if(!s_d3d.draws[i].is_clear) ++fixture_draws;
    u64 before=rsx_d3d12_backend_successful_presents();
    const int dumps_before=s_d3d.dump_frames_left;
    assert(dumps_before>0 && s_d3d.dump_skip_left==0);
    rsx_d3d12_backend_present();
    assert(rsx_d3d12_backend_successful_presents()==before+1);
    assert(s_d3d.dump_frames_left==dumps_before-1); /* a fresh copy and GPU wait */
    assert(s_d3d.readback_buf);
    void* mapped=NULL;
    D3D12_RANGE read={0,(SIZE_T)s_d3d.readback_pitch*s_d3d.height};
    assert(SUCCEEDED(s_d3d.readback_buf->lpVtbl->Map(s_d3d.readback_buf,0,&read,&mapped)) && mapped);
    const char* path=getenv("PS3RECOMP_VK_DUMP");assert(path);
    FILE* file=fopen(path,"wb");assert(file);
    fprintf(file,"P6\n%u %u\n255\n",s_d3d.width,s_d3d.height);
    u8* row=malloc(s_d3d.width*3);assert(row);
    for(u32 y=0;y<s_d3d.height;++y) {
        const u8* source=(const u8*)mapped+(size_t)y*s_d3d.readback_pitch;
        for(u32 x=0;x<s_d3d.width;++x) memcpy(row+x*3,source+x*4,3);
        assert(fwrite(row,3,s_d3d.width,file)==s_d3d.width);
    }
    free(row);assert(fclose(file)==0);
    D3D12_RANGE written={0,0};s_d3d.readback_buf->lpVtbl->Unmap(s_d3d.readback_buf,0,&written);
}
static u32 fixture_color(void) { return 0; }
static u32 fixture_guest_draws(void) { return fixture_draws; }
#define PS3RECOMP_HOST_FIXTURE_BACKEND 1
#define HOST_BACKEND_NAME "D3D12"
#define HOST_BACKEND_GUEST_SHADERS 1
#define host_backend_init fixture_init
#define host_backend_shutdown rsx_d3d12_backend_shutdown
#define host_backend_pump rsx_d3d12_backend_pump_messages
#define host_backend_present fixture_present
#define host_backend_color fixture_color
#define host_backend_center fixture_color
#define host_backend_guest_draws fixture_guest_draws
#define main unused_upstream_host_main
#include "../runtime/host/host_posix.c"
#undef main
#include "folklore_depth_program.h"

/* No title data. Every visible triangle is conditional on the real DSV depth
 * comparison. The FP reports window depth .5, so passing pixels are gray.
 * Target regions do not overlap: each Z24S8 image needs 3,686,400 bytes. */
#define CLEAR_TEST_Z24_A 0x00400000u
#define CLEAR_TEST_Z24_B 0x00800000u
#define CLEAR_TEST_Z16_C 0x00c00000u
#define CLEAR_TEST_Z16_D 0x01000000u
static unsigned clear_frame_count;

static void clear_float(u32 method, float value)
{
    u32 raw; memcpy(&raw,&value,sizeof raw); emit(method,raw);
}
static void clear_scissor(u32 x,u32 y,u32 w,u32 h)
{
    emit(NV4097_SET_SCISSOR_HORIZONTAL,x|(w<<16));
    emit(NV4097_SET_SCISSOR_VERTICAL,y|(h<<16));
}
static void clear_target(u32 offset,u32 depth_format)
{
    /* Linear/pitched A8R8G8B8, one sample; depth format 1=Z16, 2=Z24S8. */
    emit(NV4097_SET_SURFACE_FORMAT,8u|(depth_format<<5)|(1u<<8));
    emit(NV4097_SET_SURFACE_ZETA_OFFSET,offset);
    emit(NV4097_SET_SURFACE_PITCH_Z,1280u*(depth_format==1?2u:4u));
}
static void clear_depth(u32 packed_value,u32 flags)
{
    emit(NV4097_SET_ZSTENCIL_CLEAR_VALUE,packed_value);
    emit(NV4097_CLEAR_SURFACE,flags);
}
static void clear_color(void)
{
    clear_scissor(0,0,1280,720);
    emit(NV4097_SET_COLOR_CLEAR_VALUE,CLEAR_ARGB);
    emit(NV4097_CLEAR_SURFACE,0xf0);
}
static void clear_draw(void)
{
    emit(NV4097_SET_BEGIN_END,5);
    emit(NV4097_DRAW_ARRAYS,2u<<24);
    emit(NV4097_SET_BEGIN_END,0);
}
/* A half-open pixel rectangle is the entire independent oracle. Empty means
 * all background; full means all fragments passed the depth test. */
static void clear_check_gray(const char* case_name,u32 x0,u32 y0,u32 x1,u32 y1,u32 draws,int gray)
{
    const char* dump=getenv("PS3RECOMP_VK_DUMP"); assert(dump && *dump);
    guest_w32(CTRL_ADDR,g_fifo_len); cellGcm_rsx_process_fifo();
    cellGcmSetFlipCommand(0); cellGcmTickVBlank(); cellGcmTickFlip();
    cellGcm_take_flip_pending_synced(); host_backend_present();
    assert(host_backend_guest_draws()==draws);
    FILE* f=fopen(dump,"rb"); assert(f);
    char magic[3]={0}; unsigned width=0,height=0,maxval=0;
    assert(fscanf(f,"%2s %u %u %u",magic,&width,&height,&maxval)==4);
    assert(!strcmp(magic,"P6") && width==1280 && height==720 && maxval==255);
    assert(fgetc(f)=='\n');
    u8 row[1280*3]; unsigned long visible=0;
    for(u32 y=0;y<720;++y) {
        assert(fread(row,1,sizeof row,f)==sizeof row);
        for(u32 x=0;x<1280;++x) {
            const int pass=x>=x0 && x<x1 && y>=y0 && y<y1;
            const int expected[3]={pass?gray:16,pass?gray:24,pass?gray:48};
            for(u32 c=0;c<3;++c) {
                const int delta=(int)row[x*3+c]-expected[c];
                const int tolerance=pass?1:0;
                if(delta < -tolerance || delta > tolerance) {
                    fprintf(stderr,"Clear case %s (%u,%u) channel %u: got %u expected %d tolerance %d\n",
                        case_name,x,y,c,row[x*3+c],expected[c],tolerance);
                    fclose(f); exit(1);
                }
            }
            visible+=(unsigned)pass;
        }
    }
    assert(fgetc(f)==EOF); assert(fclose(f)==0);
    assert(visible==(unsigned long)(x1-x0)*(y1-y0));
    char saved[128]; snprintf(saved,sizeof saved,"clear-%02u-%s.ppm",clear_frame_count++,case_name);
    assert(rename(dump,saved)==0);
    printf("Verified %s: passing=%lu background=%lu, all 921600 RGB pixels\n",
        saved,visible,921600ul-visible);
}
static void clear_check(const char* name,u32 x0,u32 y0,u32 x1,u32 y1,u32 draws)
{
    clear_check_gray(name,x0,y0,x1,y1,draws,128);
}
static void clear_probe(const char* name,int pass)
{
    clear_draw(); clear_check(name,0,0,pass?1280:0,pass?720:0,1);
}

int main(void)
{
    /* Coordinator sets CELLMARK_DUMP>=32, CELLMARK_DUMP_EVERY=1,
     * CELLMARK_DUMP_SKIP=0 and a fresh PS3RECOMP_VK_DUMP output path. */
    vm_base=host_vm_alloc(); assert(vm_base);
    assert(host_backend_init(1280,720,"Synthetic persistent RSX clear regression")==0);
    u32 ctx=host_alloc(4,4);
    cellGcmSetupContext(ctx,CMD_SIZE,IO_SIZE,IO_ADDR,host_alloc,host_w32);
    cellGcmSetDisplayBuffer(0,0,5120,1280,720);
    /* First present has no uploaded shaders and no BEGIN at all. */
    emit(NV4097_SET_CONTEXT_DMA_COLOR_A,CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_CONTEXT_DMA_ZETA,CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_SURFACE_COLOR_AOFFSET,0);
    emit(NV4097_SET_SURFACE_COLOR_TARGET,CELL_GCM_SURFACE_TARGET_0);
    emit(NV4097_SET_SURFACE_PITCH_A,5120);
    emit(NV4097_SET_SURFACE_CLIP_HORIZONTAL,1280u<<16);
    emit(NV4097_SET_SURFACE_CLIP_VERTICAL,720u<<16);
    clear_target(CLEAR_TEST_Z24_A,2);
    clear_color(); clear_depth(0x4000005a,1);
    clear_check("standalone-before-programs",0,0,0,0,0);
    folklore_depth_program(vm_base+IO_ADDR+FP_OFFSET);
    u8 vp[32];
    rsx_test_vp_mov_out(vp,0,RSX_TEST_VP_SWZ_IDENT,0,0);
    rsx_test_vp_mov_out(vp+16,3,RSX_TEST_VP_SWZ_IDENT,1,1);
    emit_programs(vp,sizeof vp,FP_OFFSET);
    const float vertices[3][8]={{-1,-1,0,1,0,0,1,1},{3,-1,0,1,0,0,1,1},{-1,3,0,1,0,0,1,1}};
    for(u32 i=0;i<3;++i) for(u32 k=0;k<8;++k)
        guest_f32(IO_ADDR+SVTX_OFFSET+i*32+k*4,vertices[i][k]);
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET,VTX_MAIN(SVTX_OFFSET));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT,VFMT(4,32));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET+3*4,VTX_MAIN(SVTX_OFFSET+16));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT+3*4,VFMT(4,32));
    emit(NV4097_SET_CONTEXT_DMA_COLOR_A,CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_CONTEXT_DMA_ZETA,CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_SURFACE_COLOR_AOFFSET,0);
    emit(NV4097_SET_SURFACE_COLOR_TARGET,CELL_GCM_SURFACE_TARGET_0);
    emit(NV4097_SET_SURFACE_PITCH_A,5120);
    emit(NV4097_SET_SURFACE_CLIP_HORIZONTAL,1280u<<16);
    emit(NV4097_SET_SURFACE_CLIP_VERTICAL,720u<<16);
    emit(NV4097_SET_WINDOW_OFFSET,0);
    emit(NV4097_SET_VIEWPORT_HORIZONTAL,1280u<<16);
    emit(NV4097_SET_VIEWPORT_VERTICAL,720u<<16);
    clear_float(0x0a20,640); clear_float(0x0a24,360); clear_float(0x0a28,.5f); clear_float(0x0a2c,0);
    clear_float(0x0a30,640); clear_float(0x0a34,360); clear_float(0x0a38,.5f); clear_float(0x0a3c,0);
    clear_float(NV4097_SET_CLIP_MIN,0); clear_float(NV4097_SET_CLIP_MAX,1);
    emit(NV4097_SET_ZMIN_MAX_CONTROL,1);
    emit(NV4097_SET_DEPTH_TEST_ENABLE,1);
    emit(NV4097_SET_DEPTH_FUNC,0x0201); /* LESS */
    emit(NV4097_SET_DEPTH_MASK,0); /* Draws must never replace the test depth. */
    emit(NV4097_SET_BLEND_ENABLE,0);
    emit(NV4097_SET_STENCIL_TEST_ENABLE,0);
    emit(0x032c,0xff); /* Front stencil write mask, independent of test mask. */
    emit(NV4097_SET_COLOR_MASK,0x01010101);
    emit(NV4097_SET_SHADER_WINDOW,720);

    clear_target(CLEAR_TEST_Z24_A,2);
    clear_color(); clear_depth(0x4000005a,1);
    clear_probe("before-first-begin",0);

    clear_color(); clear_probe("color-only-preserves-depth",0);

    clear_color(); emit(NV4097_SET_COLOR_CLEAR_VALUE,0xffffffff);
    clear_depth(0xc00000a5,1);
    clear_check("depth-only-preserves-color",0,0,0,0,0);

    clear_color(); clear_probe("depth-only-stored",1);
    clear_color(); clear_probe("across-present",1);

    clear_color(); clear_depth(0x40000012,1); clear_depth(0xc0000034,1);
    clear_probe("last-clear-high",1);
    clear_color(); clear_depth(0xc0000034,1); clear_depth(0x40000012,1);
    clear_probe("last-clear-low",0);

    clear_color(); clear_depth(0x40000012,1);
    clear_scissor(173,97,311,223); clear_depth(0xc0000034,1);
    clear_scissor(0,0,1280,720); clear_draw();
    clear_check("exact-scissor",173,97,484,320,1);
    clear_color(); clear_draw(); clear_check("scissor-persists",173,97,484,320,1);

    clear_color(); clear_depth(0x40000012,1);
    clear_scissor(1199,631,300,200); clear_depth(0xc0000034,1);
    clear_scissor(0,0,1280,720); clear_draw();
    clear_check("scissor-surface-intersection",1199,631,1280,720,1);

    clear_color(); clear_depth(0x40000012,1);
    clear_scissor(123,45,0,200); clear_depth(0xc0000034,1);
    clear_scissor(0,0,1280,720); clear_probe("empty-clear",0);

    clear_color(); clear_depth(0x40000012,1);
    clear_target(CLEAR_TEST_Z24_B,2); clear_depth(0xc0000034,1);
    clear_target(CLEAR_TEST_Z24_A,2); clear_probe("switch-before-begin-A",0);
    clear_target(CLEAR_TEST_Z24_B,2); clear_color(); clear_probe("switch-after-present-B",1);
    clear_target(CLEAR_TEST_Z24_A,2); clear_color(); clear_probe("return-after-present-A",0);

    /* Poison upper bits so decoding Z16 as high 24 bits reverses the result. */
    clear_target(CLEAR_TEST_Z16_C,1); clear_color(); clear_depth(0x0000c000,1);
    clear_probe("z16-low16-high",1);
    clear_target(CLEAR_TEST_Z16_D,1); clear_color(); clear_depth(0xffff4000,1);
    clear_probe("z16-low16-low",0);
    clear_target(CLEAR_TEST_Z16_C,1); clear_color(); clear_probe("z16-return-C",1);
    clear_target(CLEAR_TEST_Z24_A,2); clear_color(); clear_probe("z24-independent-A",0);

    clear_color(); clear_depth(0xffffffaa,2); clear_probe("stencil-only-keeps-low-depth",0);
    clear_target(CLEAR_TEST_Z24_B,2); clear_color(); clear_depth(0x00000055,2);
    clear_probe("stencil-only-keeps-high-depth",1);

    clear_target(CLEAR_TEST_Z24_A,2);
    emit(NV4097_SET_COLOR_MASK,0); clear_color();
    emit(NV4097_SET_COLOR_MASK,0x01010101);
    clear_probe("clear-ignores-draw-color-mask",0);

    /* If the middle clear moves before both draws, neither passes. If it
     * moves after both, the .5 draw replaces the first gray64 with gray128. */
    clear_color(); clear_depth(0xc0000012,1);
    clear_float(0x0a28,.25f); clear_draw();
    clear_depth(0x19999934,1);
    clear_float(0x0a28,.5f); clear_draw();
    clear_check_gray("draw-clear-draw-order",0,0,1280,720,2,64);

    /* Same guest programs and target: only the depth write state changes.
     * An enabled first write stores .25 and rejects the later .5 draw. */
    clear_color(); clear_depth(0xc0000012,1);
    emit(NV4097_SET_DEPTH_MASK,1);
    clear_float(0x0a28,.25f); clear_draw();
    emit(NV4097_SET_DEPTH_MASK,0);
    clear_float(0x0a28,.5f); clear_draw();
    clear_check_gray("depth-write-enabled",0,0,1280,720,2,64);

    /* With writes disabled, .75 remains stored and the second draw passes. */
    clear_color(); clear_depth(0xc0000012,1);
    clear_float(0x0a28,.25f); clear_draw();
    clear_float(0x0a28,.5f); clear_draw();
    clear_check("depth-write-disabled",0,0,1280,720,2);

    clear_color(); clear_depth(0x40000012,1);
    emit(NV4097_SET_DEPTH_FUNC,0x0204); /* GREATER */
    clear_probe("depth-func-greater",1);
    clear_color(); emit(NV4097_SET_DEPTH_FUNC,0x0201);
    clear_probe("depth-func-less-restored",0);

    clear_color(); emit(NV4097_SET_DEPTH_TEST_ENABLE,0);
    clear_probe("depth-test-disabled",1);
    clear_color(); emit(NV4097_SET_DEPTH_TEST_ENABLE,1);
    clear_probe("depth-test-restored",0);

    assert(clear_frame_count==29);
    host_backend_shutdown(); free(vm_base);
    puts("All 29 persistent-clear frames verified; synthetic D3D12 evidence only");
    return 0;
}


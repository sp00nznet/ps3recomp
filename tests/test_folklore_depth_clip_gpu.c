/* Synthetic guest FIFO, VP and FP only. This fixture checks every saved RGB
 * pixel against an independent window-depth oracle. The coordinator runs it
 * serially with the GPU environment used by the existing WPOS fixture. */
#define main unused_upstream_host_main
#include "../runtime/host/host_posix.c"
#undef main
#include "folklore_depth_program.h"
#include <assert.h>
#include <math.h>

static void depth_emit_float(u32 method, float value)
{
    u32 raw; memcpy(&raw, &value, sizeof raw); emit(method, raw);
}
static void depth_upload_triangle(float z0, float z1, float z2, float w)
{
    const float v[3][8] = {
        {-w,-w,z0,w, 0,0,1,1},
        {3*w,-w,z1,w, 0,0,1,1},
        {-w,3*w,z2,w, 0,0,1,1}
    };
    for (unsigned i=0;i<3;++i)
        for (unsigned k=0;k<8;++k)
            guest_f32(IO_ADDR+SVTX_OFFSET+i*32+k*4,v[i][k]);
}
/* kind=0 constant covered depth; kind=1 completely clipped; kind=2 near
 * crossing: D=(pixel_x+0.5)/(2*1280), so exactly the right half survives.
 * This formula is from the fixture's triangle plane, independent of the
 * production normalization helper and the requested viewport endpoints. */
static void depth_verify_ppm(const char* path, int kind, double constant_depth)
{
    FILE* f=fopen(path,"rb"); assert(f);
    char magic[3]={0}; unsigned width=0,height=0,maxval=0;
    assert(fscanf(f,"%2s %u %u %u",magic,&width,&height,&maxval)==4);
    assert(!strcmp(magic,"P6") && width==1280 && height==720 && maxval==255);
    int delimiter=fgetc(f); assert(delimiter=='\n');
    unsigned char row[1280*3]; unsigned long covered=0,clear=0;
    for(unsigned y=0;y<720;++y) {
        assert(fread(row,1,sizeof row,f)==sizeof row);
        for(unsigned x=0;x<1280;++x) {
            const double d=kind==2 ? ((double)x+0.5)/2560.0 : constant_depth;
            const int visible=kind==0 || (kind==2 && x>=640);
            const int gray=(int)floor(d*255.0+0.5);
            const int expected[3]={visible?gray:16,visible?gray:24,visible?gray:48};
            for(unsigned c=0;c<3;++c) {
                const int delta=(int)row[3*x+c]-expected[c];
                const int tolerance=visible?1:0; /* UNORM rounding only */
                if(delta < -tolerance || delta > tolerance) {
                    fprintf(stderr,"Depth pixel mismatch %s (%u,%u) channel %u: got %u expected %d tolerance %d\n",
                        path,x,y,c,row[3*x+c],expected[c],tolerance);
                    fclose(f); exit(1);
                }
            }
            if(visible) ++covered; else ++clear;
        }
    }
    assert(fgetc(f)==EOF); fclose(f);
    assert(covered+clear==921600ul);
    if(kind==2) assert(covered==460800ul && clear==460800ul);
    printf("Verified %s: covered=%lu clear=%lu, all 921600 RGB pixels\n",path,covered,clear);
}
int main(int argc,char** argv)
{
    static const struct {
        const char* name; float near_z,far_z,scale,offset,z0,z1,z2,w;
        int kind; double expected_depth;
    } frames[]={
        {"inside", .25f,.75f,.5f,.5f, -.25f,-.25f,-.25f,1, 0,.375},
        {"below", .25f,.75f,.5f,.5f, -.75f,-.75f,-.75f,1, 1,0},
        {"above", .25f,.75f,.5f,.5f, .75f,.75f,.75f,1, 1,0},
        {"cross-near", .25f,.75f,.5f,.5f, -1,1,-1,1, 2,0},
        {"range-change", .125f,.625f,.5f,.5f, -.25f,-.25f,-.25f,1, 0,.375},
        {"zero-scale", .25f,.75f,0,.375f, 123,123,123,1, 0,.375},
        {"negative-scale", .25f,.75f,-.5f,.5f, .25f,.25f,.25f,1, 0,.375},
        {"w-two", .25f,.75f,.5f,.5f, -.5f,-.5f,-.5f,2, 0,.375},
        {"default-range", 0,1,.5f,.5f, -.25f,-.25f,-.25f,1, 0,.375},
        {"inside-again", .25f,.75f,.5f,.5f, -.25f,-.25f,-.25f,1, 0,.375},
        {"cross-again", .25f,.75f,.5f,.5f, -1,1,-1,1, 2,0}
    };
    const char* dump=getenv("PS3RECOMP_VK_DUMP");
    assert(dump && *dump); /* Also set PS3RECOMP_VK_DUMP_EVERY=1. */
    vm_base=host_vm_alloc(); assert(vm_base);
    assert(host_backend_init(1280,720,"Synthetic depth-clip regression")==0);
    assert(HOST_BACKEND_GUEST_SHADERS);
    u32 ctx=host_alloc(4,4);
    cellGcmSetupContext(ctx,CMD_SIZE,IO_SIZE,IO_ADDR,host_alloc,host_w32);
    cellGcmSetDisplayBuffer(0,0,5120,1280,720);
    emit(NV4097_SET_SURFACE_COLOR_TARGET,CELL_GCM_SURFACE_TARGET_0);
    emit(NV4097_SET_SURFACE_FORMAT,8u|(1u<<8));
    emit(NV4097_SET_SURFACE_PITCH_A,5120);
    folklore_depth_program(vm_base+IO_ADDR+FP_OFFSET);
    u8 vp[32];
    rsx_test_vp_mov_out(vp,0,RSX_TEST_VP_SWZ_IDENT,0,0);
    rsx_test_vp_mov_out(vp+16,3,RSX_TEST_VP_SWZ_IDENT,1,1);
    emit_programs(vp,sizeof vp,FP_OFFSET);
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET,VTX_MAIN(SVTX_OFFSET));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT,VFMT(4,32));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET+3*4,VTX_MAIN(SVTX_OFFSET+16));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT+3*4,VFMT(4,32));
    emit(NV4097_SET_VIEWPORT_HORIZONTAL,1280u<<16);
    emit(NV4097_SET_VIEWPORT_VERTICAL,720u<<16);
    depth_emit_float(0x0a20,640); depth_emit_float(0x0a24,360); depth_emit_float(0x0a2c,0);
    depth_emit_float(0x0a30,640); depth_emit_float(0x0a34,360); depth_emit_float(0x0a3c,0);
    emit(NV4097_SET_ZMIN_MAX_CONTROL,1);
    emit(NV4097_SET_DEPTH_TEST_ENABLE,0); emit(NV4097_SET_BLEND_ENABLE,0);
    emit(NV4097_SET_SHADER_WINDOW,0x2d0);
    if(argc==2) {
        depth_upload_triangle(0,0,0,1);
        if(!strcmp(argv[1],"invalid-range")) depth_emit_float(NV4097_SET_CLIP_MIN,1);
        else if(!strcmp(argv[1],"invalid-control")) emit(NV4097_SET_ZMIN_MAX_CONTROL,0);
        else return 2;
        emit(NV4097_SET_BEGIN_END,5);emit(NV4097_DRAW_ARRAYS,2u<<24);emit(NV4097_SET_BEGIN_END,0);
        guest_w32(CTRL_ADDR,g_fifo_len);cellGcm_rsx_process_fifo();return 99;
    }
    for(unsigned i=0;i<sizeof frames/sizeof frames[0];++i) {
        depth_upload_triangle(frames[i].z0,frames[i].z1,frames[i].z2,frames[i].w);
        depth_emit_float(0x0a38,frames[i].scale); depth_emit_float(0x0a28,frames[i].offset);
        depth_emit_float(NV4097_SET_CLIP_MIN,frames[i].near_z);
        depth_emit_float(NV4097_SET_CLIP_MAX,frames[i].far_z);
        /* Keep the explicitly programmed per-frame Z transform: the generic
         * host fixture's submit_frame intentionally resets it to identity. */
        emit(NV4097_SET_COLOR_CLEAR_VALUE,CLEAR_ARGB);
        emit(NV4097_CLEAR_SURFACE,0xF0u);
        emit(NV4097_SET_BEGIN_END,5);
        emit(NV4097_DRAW_ARRAYS,2u<<24);
        emit(NV4097_SET_BEGIN_END,0);
        guest_w32(CTRL_ADDR,g_fifo_len);cellGcm_rsx_process_fifo();
        cellGcmSetFlipCommand(0); cellGcmTickVBlank(); cellGcmTickFlip();
        cellGcm_take_flip_pending_synced(); host_backend_present();
        assert(host_backend_guest_draws()==1);
        char name[96]; snprintf(name,sizeof name,"depth-%02u-%s.ppm",i,frames[i].name);
        assert(rename(dump,name)==0);
        depth_verify_ppm(name,frames[i].kind,frames[i].expected_depth);
    }
    host_backend_shutdown(); free(vm_base);
    puts("All 11 depth-clip frames verified with independent pixel oracles");
    return 0;
}

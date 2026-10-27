/* Exercise production depth-to-guest storage through actual texture lookup,
 * dirty tracking and cache refresh; expected quantized bytes are literal.
 * No game programs, original assets or graphics API are used. */
#define main original_engine_test_main
#include "test_rsx_draw_engine.c"
#undef main
#include <math.h>

#define PACK_W 12u
#define PACK_H 2u
#define PACK_PITCH 80u
static float samples[PACK_W];
static const u32 expected24[PACK_W]={0xFFFFFF,0xFFFFFE,0xFFFFFD,0x800000,0,0,0,0xFFFFFF,0xFFFFFF,0,0xBFFFFF,0x400000};
static const u16 expected16[PACK_W]={0xFFFF,0xFFFF,0xFFFF,0x8000,0,0,0,0xFFFF,0xFFFF,0,0xBFFF,0x4000};
static u8 uploaded[PACK_W*PACK_H*4];
static rsx_be_format uploaded_format;
static int changed,read_w,read_h;
static u32 sample_at(u32 x,u32 y) { return (y+changed)&1u ? PACK_W-1u-x:x; }
static int varied_depth(void* user,u32 depth,u32 x,u32 y,u32 w,u32 h,float* out,u32 pitch)
{
    (void)user;(void)depth;stub.n_depth_readback++;read_w=w;read_h=h;
    if(x || y || w!=PACK_W || h!=PACK_H || pitch!=PACK_W*sizeof(float))return -1;
    for(u32 row=0;row<h;row++)for(u32 col=0;col<w;col++)
        *(float*)((u8*)out+row*pitch+col*sizeof(float))=samples[sample_at(col,row)];
    return 0;
}
static u32 capture_create(void* user,rsx_be_format fmt,u32 w,u32 h,u32 mips,u32 faces,u32 remap,u32 rsx_fmt)
{
    uploaded_format=fmt;return stub_texture_create(user,fmt,w,h,mips,faces,remap,rsx_fmt);
}
static void capture_upload(void* user,u32 texture,u32 face,u32 mip,u32 w,u32 h,const void* data,u32 pitch,u32 rows)
{
    stub_texture_upload(user,texture,face,mip,w,h,data,pitch,rows);
    u32 bpp=uploaded_format==RSX_BE_FMT_R16?2u:4u;
    if(mip || w!=PACK_W || h!=PACK_H || pitch!=PACK_W*bpp || rows!=PACK_H)return;
    for(u32 y=0;y<h;y++)memcpy(uploaded+y*PACK_W*bpp,(const u8*)data+y*pitch,PACK_W*bpp);
}
static void check_guest(u32 bpp)
{
    const u8* p=vm_base+GUEST_LOCAL_EA+ZETA_OFFSET;
    for(u32 y=0;y<PACK_H;y++)for(u32 x=0;x<PACK_W;x++){
        u32 want=bpp==4?expected24[sample_at(x,y)]:expected16[sample_at(x,y)];
        const u8* q=p+y*PACK_PITCH+x*bpp;
        int ok=bpp==4?(q[0]==(want>>16) && q[1]==((want>>8)&255) && q[2]==(want&255) && q[3]==0):(q[0]==(want>>8) && q[1]==(want&255));
        CHECK(ok,"depth%u pixel(%u,%u) has literal big-endian depth %06X and zero stencil (actual %02X %02X %02X %02X)",bpp==4?24:16,x,y,want,q[0],q[1],q[2],q[3]);
    }
    int intact=1;
    for(int i=-16;i<(int)(PACK_PITCH*3);i++){
        int used=i>=0 && i<(int)(PACK_PITCH*PACK_H) && i%(int)PACK_PITCH<(int)(PACK_W*bpp);
        if(!used)intact &= p[i]==0xA6;
    }
    CHECK(intact,"depth%u preserves prefix, row padding and rows outside used ROI",bpp==4?24:16);
}
static void check_upload(u32 bpp)
{
    int ok=1;
    for(u32 y=0;y<PACK_H;y++)for(u32 x=0;x<PACK_W;x++){
        u32 want=bpp==4?expected24[sample_at(x,y)]:expected16[sample_at(x,y)];
        const u8* q=uploaded+(y*PACK_W+x)*bpp;
        if(bpp==4)ok &= q[0]==((want>>8)&255) && q[1]==(want&255) && q[2]==0 && q[3]==(want>>16);
        else {u16 got;memcpy(&got,q,sizeof got);ok &= got==want;}
    }
    CHECK(ok,"depth%u texture upload decodes the newly stored guest bytes",bpp==4?24:16);
}
static void packing_case(u32 bpp)
{
    rsx_draw_backend be=g_stub_backend;be.depth_readback=varied_depth;be.texture_create=capture_create;be.texture_upload=capture_upload;
    memset(&stub,0,sizeof stub);rsx_draw_engine_set_backend(&be);
    CHECK(rsx_draw_engine_init(32,8)==0,"packing engine initializes");
    m(M_CONTEXT_DMA_COLOR_A,DMA_LOCAL);m(M_SURFACE_CLIP_H,PACK_W<<16);m(M_SURFACE_CLIP_V,PACK_H<<16);
    m(M_VTXFMT,2u|(4u<<4)|(16u<<8));m(M_VTXBUF_OFFSET,0x80000000u|VTX_OFFSET);
    m(M_CONTEXT_DMA_ZETA,DMA_LOCAL);m(M_SURFACE_FORMAT,8u|((bpp==4?2u:1u)<<5));
    m(M_ZETA_OFFSET,ZETA_OFFSET);m(M_ZETA_PITCH,PACK_PITCH);m(M_COLOR_A_OFFSET,0x90000u);
    u8* guest=vm_base+GUEST_LOCAL_EA+ZETA_OFFSET;memset(guest-16,0xA6,PACK_PITCH*3+16);
    bind_texture_unit(RSX_LOCATION_LOCAL,ZETA_OFFSET);m(M_TEX_UNIT+0x18,(PACK_W<<16)|PACK_H);m(0x1840,PACK_PITCH);
    if(bpp==2)m(M_TEX_UNIT+0x04,1u|(2u<<4)|(0xB2u<<8)|(1u<<16));
    changed=0;draw_triangle();const u32 cached=stub.bound_tex[0];
    CHECK(cached && stub.n_depth_readback==0,"alias texture is cached before any depth write");
    for(int pass=0;pass<2;pass++){
        changed=pass;m(M_TEX_UNIT+0x0C,0);m(M_COLOR_A_OFFSET,ZETA_OFFSET);m(M_COLOR_MASK,0);m(M_DEPTH_TEST,1);m(M_DEPTH_WRITE,1);draw_triangle();
        m(M_COLOR_A_OFFSET,0x90000u);m(M_COLOR_MASK,0x01010101);m(M_DEPTH_TEST,0);m(M_DEPTH_WRITE,0);m(M_TEX_UNIT+0x0C,0x80000000u);
        const u32 before=stub.bound_tex[0];const int uploads=stub.n_texture_upload;draw_triangle();
        CHECK(stub.n_depth_readback==pass+1 && read_w==PACK_W && read_h==PACK_H,"dirty depth readback uses only the used ROI");
        CHECK(stub.n_texture_upload==uploads+1 && stub.bound_tex[0]!=before,"same-frame guest-byte change refreshes cached alias");
        CHECK(uploaded_format==(bpp==4?RSX_BE_FMT_R8G8B8A8:RSX_BE_FMT_R16),"alias upload uses its descriptor format");
        check_guest(bpp);check_upload(bpp);
        const u32 stable=stub.bound_tex[0];const int stable_uploads=stub.n_texture_upload;draw_triangle();
        CHECK(stub.n_depth_readback==pass+1 && stub.n_texture_upload==stable_uploads && stub.bound_tex[0]==stable,"unchanged depth reuses alias without another readback");
    }
    m(M_ZSTENCIL_CLEAR,bpp==4?0xFFFFFF00u:0xFFFFu);m(M_CLEAR_BUFFERS,1u);
    CHECK(stub.last_clear_depth==1.0f,"far-plane clear decoding remains unchanged");
    engine_down();
}
int main(void)
{
    vm_base=calloc(1,GUEST_BYTES);if(!vm_base)return 1;ppu_vm_size=GUEST_BYTES;
    for(u32 i=0;i<12;i++){u8* p=vm_base+VTX_OFFSET+i*4;p[0]=0x3F;p[1]=0x80;}
    samples[0]=1;samples[1]=nextafterf(1,0);samples[2]=nextafterf(samples[1],0);samples[3]=.5f;samples[4]=0;samples[5]=NAN;samples[6]=-.25f;samples[7]=1.25f;samples[8]=INFINITY;samples[9]=-INFINITY;samples[10]=.75f;samples[11]=.25f;
    packing_case(4);packing_case(2);free(vm_base);
    printf("depth packing: %d failures\n",g_failures);return g_failures?1:0;
}

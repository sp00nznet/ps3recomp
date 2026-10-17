/* Real GPU regression: per-draw VS/PS snapshots survive caller reuse and
 * subsequent binds. Exercise both the batch ring and shared-UBO fallback. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
uint8_t* vm_base;
static unsigned checks;
#define CHECK(v) do { ++checks; if (!(v)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#v); return 1; } } while (0)
static void option(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name,value);
#else
    setenv(name,value,1);
#endif
}
int main(int argc, char** argv) {
    SDL_SetMainReady(); option("PS3RECOMP_VK_GUEST_PROGRAMS","1");
    option("PS3RECOMP_VK_BATCH",argc>1?argv[1]:"1");
    CHECK(rsx_vulkan_backend_init(8,4,"Constant snapshots regression")==0);
    const char* vs="cbuffer V:register(b0){float4 offset;} float4 main(float4 p:ATTR0):SV_Position{return p+offset;}";
    const char* ps="cbuffer P:register(b1){float4 color;} float4 main():SV_Target0{return color;}";
    rsx_vertex_layout_plan layout={0};layout.count=1;layout.stride=16;
    rsx_be_render_state state={0};state.color_mask=0x01010101u;
    state.antialias_control=0xFFFF0001u;
    const u32 pipe=vk_eng_pipeline_create(NULL,vs,ps,&state,&layout,16,RSX_BE_FMT_R8G8B8A8,1);
    const u32 color=vk_eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,8,4,NULL,0);
    CHECK(pipe&&color); vk_eng_bind_targets(NULL,&color,1,0);vk_eng_bind_pipeline(NULL,pipe);
    const float clear[4]={0,0,1,1};vk_eng_clear_color(NULL,color,clear);
    const float verts[3][4]={{-1,-1,.25f,1},{3,-1,.25f,1},{-1,3,.25f,1}};
    for (u32 half=0;half<2;half++) {
        float offset[4]={0,0,0,0};float rgba[4]={half?0:1,half?1:0,0,1};
        vk_eng_bind_vs_constants(NULL,offset,sizeof offset);
        vk_eng_bind_ps_constants(NULL,rgba,sizeof rgba);
        /* The inputs may be reused immediately after bind. */
        offset[0]=100;rgba[0]=rgba[1]=rgba[2]=0;
        vk_eng_set_scissor(NULL,half*4,0,4,4);
        vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,verts,3,16,NULL,0);
    }
    /* A later bind must not alter either submitted draw. */
    float other[4]={0,0,1,1};vk_eng_bind_ps_constants(NULL,other,sizeof other);
    u8 pixels[8*4*4];CHECK(vk_eng_readback_rows(color,0,0,8,4,pixels,8*4)==0);
    for (u32 y=0;y<4;y++) for(u32 x=0;x<8;x++) {
        const u8* p=pixels+(y*8+x)*4;
        if (p[0]!=(x<4?255:0)||p[1]!=(x<4?0:255)||p[2]!=0||p[3]!=255)
            fprintf(stderr,"pixel %u,%u = %u %u %u %u\n",x,y,p[0],p[1],p[2],p[3]);
        CHECK(p[0]==(x<4?255:0)&&p[1]==(x<4?0:255)&&p[2]==0&&p[3]==255);
    }
    CHECK(s_e2.constant_shadow!=NULL);
    vk_eng2_shutdown(NULL);CHECK(s_e2.constant_shadow==NULL);
    rsx_vulkan_backend_shutdown();
    printf("PASS %u GPU constant snapshot checks, batch=%s\n",checks,argc>1?argv[1]:"1");
    return 0;
}

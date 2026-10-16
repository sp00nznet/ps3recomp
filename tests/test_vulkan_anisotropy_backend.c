/* Actual backend sampler-cache checks; --gpu additionally renders an A/B fixture. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
uint8_t* vm_base;
static unsigned checks, created;
static VkSamplerCreateInfo recorded[32];
#define CHECK(v) do { ++checks; if (!(v)) { fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#v); return 1; } } while(0)
static VKAPI_ATTR VkResult VKAPI_CALL fake_create(VkDevice dev, const VkSamplerCreateInfo* info,
                                                 const VkAllocationCallbacks* alloc, VkSampler* sampler)
{
    (void)dev; (void)alloc;
    if (created >= 32) return VK_ERROR_TOO_MANY_OBJECTS;
    recorded[created] = *info;
    *sampler = (VkSampler)(uintptr_t)(++created);
    return VK_SUCCESS;
}
static int cpu_checks(void)
{
    pvkCreateSampler = fake_create;
    s_vk.anisotropy_ok = 1; s_vk.max_anisotropy = 16;
    rsx_be_sampler_desc d = {0};
    d.min_linear = d.mag_linear = d.mip_linear = d.mip_present = 1;
    d.wrap_s = d.wrap_t = d.wrap_r = 1; d.max_lod = 8;
    const VkSampler off = vk_eng_sampler(&d);
    CHECK(off && created == 1 && !recorded[0].anisotropyEnable && recorded[0].maxAnisotropy == 1);
    d.anisotropy_code = 7;
    const VkSampler full = vk_eng_sampler(&d);
    CHECK(full != off && created == 2 && recorded[1].anisotropyEnable && recorded[1].maxAnisotropy == 16);
    CHECK(vk_eng_sampler(&d) == full && created == 2);
    s_vk.max_anisotropy = 8;
    const VkSampler capped = vk_eng_sampler(&d);
    CHECK(capped != full && created == 3 && recorded[2].maxAnisotropy == 8);
    d.anisotropy_code = 4;
    CHECK(vk_eng_sampler(&d) == capped && created == 3);
    s_vk.anisotropy_ok = 0;
    CHECK(vk_eng_sampler(&d) == off && created == 3);
    s_vk.anisotropy_ok = 1; s_vk.max_anisotropy = 16; s_vk.anisotropy_override = 16;
    d.anisotropy_code = 0;
    CHECK(vk_eng_sampler(&d) == full);
    d.vertex_sampler = 1;
    CHECK(vk_eng_sampler(&d) == off);
    d.vertex_sampler = 0; d.mag_linear = 0;
    const VkSampler nearest = vk_eng_sampler(&d);
    CHECK(nearest && created == 4 && !recorded[3].anisotropyEnable);

    /* Legacy/vtable path must distinguish the guest's aniso bits in its key. */
    s_vk.anisotropy_override = 0;
    rsx_texture_state t = {0}; t.filter = (6u << 16) | (2u << 24);
    t.address = 0x010101u; t.control0 = 2048u << 7;
    const VkSampler legacy_off = vk_unit_sampler(&t);
    CHECK(legacy_off && created == 5 && !recorded[4].anisotropyEnable);
    t.control0 |= 7u << 4;
    const VkSampler legacy_on = vk_unit_sampler(&t);
    CHECK(legacy_on != legacy_off && created == 6 && recorded[5].maxAnisotropy == 16);
    CHECK(vk_unit_sampler(&t) == legacy_on && created == 6);
    memset(&s_vk, 0, sizeof s_vk); memset(s_esamp, 0, sizeof s_esamp); s_esamp_count = 0;
    return 0;
}
static int gpu_checks(void)
{
    SDL_SetMainReady();
    CHECK(rsx_vulkan_backend_init(64,64,"Anisotropy regression") == 0);
    CHECK(s_vk.anisotropy_override == 0 && s_render_scale == 1);
    if (!s_vk.anisotropy_ok || s_vk.max_anisotropy < 2) {
        puts("SKIP GPU anisotropy unavailable"); rsx_vulkan_backend_shutdown(); return 77;
    }
    static const char vs[] = "float4 main(float4 p:ATTR0):SV_Position{return p;}";
    static const char ps[] = "Texture2D tex0:register(t0); SamplerState samp0:register(s0);\n"
        "struct PSInput { float4 position:SV_POSITION; };\n"
        "float4 main(PSInput input):SV_Target0 {\n/* RSX shader window */\n"
        "input.position.xy=input.position.xy*float2(1.0,-1.0)+float2(0.0,64.0);\n"
        "return tex0.Sample(samp0,input.position.xy/64.0*float2(13.37,0.17)+float2(0.003,0.213));}";
    rsx_vertex_layout_plan layout = {0}; layout.count=1; layout.stride=16; layout.attrs[0]=0;
    rsx_be_render_state state={0}; state.color_mask=0x01010101u; state.antialias_control=0xFFFF0001u;
    const u32 pipe=vk_eng_pipeline_create(NULL,vs,ps,&state,&layout,16,RSX_BE_FMT_R8G8B8A8,1);
    const u32 target=vk_eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,64,64,NULL,0);
    const u32 texture=vk_eng_texture_create(NULL,RSX_BE_FMT_R8G8B8A8,64,64,1,1,0xAAE4u,0x85u);
    CHECK(pipe && target && texture);
    u8 pattern[64*64*4];
    for(u32 y=0;y<64;++y) for(u32 x=0;x<64;++x) {
        u8* p=pattern+(y*64+x)*4; p[0]=p[1]=p[2]=(x&1u)?255:0; p[3]=255;
    }
    vk_eng_texture_upload(NULL,texture,0,0,64,64,pattern,64*4,64);
    u32 textures[RSX_BE_MAX_TEXTURES]={0}; textures[0]=texture;
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES]={{0}};
    samplers[0].min_linear=samplers[0].mag_linear=1;
    samplers[0].wrap_s=samplers[0].wrap_t=samplers[0].wrap_r=1;
    const float vertices[3][4]={{-1,-1,.25f,1},{3,-1,.25f,1},{-1,3,.25f,1}};
    u8 pixels[2][64*64*4];
    for(u32 pass=0;pass<2;++pass) {
        samplers[0].anisotropy_code=pass?7:0;
        const float clear[4]={1,0,1,1}; vk_eng_clear_color(NULL,target,clear);
        vk_eng_bind_targets(NULL,&target,1,0); vk_eng_bind_pipeline(NULL,pipe);
        vk_eng_bind_textures(NULL,textures,samplers,1);
        vk_eng_set_viewport(NULL,0,0,64,64,0,1); vk_eng_set_scissor(NULL,0,0,64,64);
        vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,vertices,3,16,NULL,0);
        CHECK(vk_eng_readback_rows_raw(target,0,0,64,64,pixels[pass],64*4)==0);
        CHECK(rsx_dump_ppm(pass?"anisotropy-on.ppm":"anisotropy-off.ppm",pixels[pass],64,64)==0);
    }
    unsigned changed=0;
    for(u32 i=0;i<64*64;++i) {
        CHECK(pixels[0][i*4]==pixels[0][i*4+1] && pixels[1][i*4]==pixels[1][i*4+1]);
        if(memcmp(pixels[0]+i*4,pixels[1]+i*4,4)) ++changed;
    }
    CHECK(changed>64);
    printf("Anisotropy actual GPU sampling: changed_pixels=%u/4096 device_max=%g\n",changed,(double)s_vk.max_anisotropy);
    rsx_vulkan_backend_shutdown(); return 0;
}
int main(int argc,char** argv)
{
    if(argc!=2 || (strcmp(argv[1],"--cpu") && strcmp(argv[1],"--gpu"))) return 2;
    if(cpu_checks()) return 1;
    if(!strcmp(argv[1],"--gpu")) { const int result=gpu_checks(); if(result) return result; }
    printf("Vulkan anisotropy backend: %u checks passed (%s)\n",checks,argv[1]); return 0;
}

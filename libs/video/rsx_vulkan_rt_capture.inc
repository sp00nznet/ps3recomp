/* Live CPU snapshots, owned by the rendering thread. No files or prior-frame
 * meshes are inputs. Rendering and shader execution remain with the caller. */
#include "rsx_scene_epoch.h"
#include "rsx_scene_alpha.h"
#include "rsx_authored_lights.h"
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif
/* Use the operating system SHA256 implementation. Hashing occurs only when an
 * explicitly requested rayquery pipeline is created, never per draw. */
static int vk_rt_source_sha256(const char* source,char hex[65])
{
    if(!hex)return 0;
    hex[0]=0;
#ifdef _WIN32
    if(!source)return 0;
    size_t length=strlen(source);if(length>=262144)return 0;
    BCRYPT_ALG_HANDLE algorithm=NULL;BCRYPT_HASH_HANDLE hash=NULL;
    unsigned char digest[32];int result=0;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,NULL,0)<0)return 0;
    if(BCryptCreateHash(algorithm,&hash,NULL,0,NULL,0,0)>=0 &&
       BCryptHashData(hash,(PUCHAR)source,(ULONG)length,0)>=0 &&
       BCryptFinishHash(hash,digest,sizeof digest,0)>=0) {
        static const char digits[]="0123456789abcdef";
        for(unsigned i=0;i<32;++i){hex[i*2]=digits[digest[i]>>4];hex[i*2+1]=digits[digest[i]&15];}
        hex[64]=0;result=1;
    }
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    return result;
#else
    (void)source;return 0; /* No unverified digest implementation fallback. */
#endif
}
static int vk_rt_shader_contract(const char* source,u32* mask,u32* bytes)
{
    char hex[65];*mask=0;*bytes=0;
    if(!vk_rt_source_sha256(source,hex))return -1;
    const rsx_scene_alpha_contract* contract=rsx_scene_alpha_contract_for_sha256(hex);
    int result=rsx_scene_alpha_contract_id(contract);
    if(result>=0){*mask=rsx_scene_alpha_texture_mask(contract);*bytes=(u32)rsx_scene_alpha_required_fp_bytes(contract);}
    return result;
}
/* Same ordinary, nondegenerate depth interval supported by rsx_depth_clip. */
static int vk_rt_depth_range(float n,float f)
{
    return isfinite(n) && isfinite(f) && n>=0.0f && f<=1.0f && f-n>0.000001f;
}
#define VK_RT_EPOCHS 4u
#define VK_RT_TOTAL_BYTES (128u*1024u*1024u)
static struct {
    rsx_scene_epoch* epochs[VK_RT_EPOCHS];
    u64 frame,generation;
    int overflow;
    u64 copied_bytes,draws,rejected,copy_ticks;
    u8 rejected_pipeline_logged[VK_ENG_MAX_PIPE];
} s_rt_capture;
static rsx_scene_epoch* vk_rt_epoch(u32 depth)
{
    for(unsigned i=0;i<VK_RT_EPOCHS;++i)
        if(s_rt_capture.epochs[i] && s_rt_capture.epochs[i]->depth==depth)return s_rt_capture.epochs[i];
    return NULL;
}
static void vk_rt_capture_clear(u32 depth,float value)
{
    vk_eng_obj* o=vk_eng_get(depth,VK_ENG_DEPTH);
    if(!s_vk.rt_enabled || !o || o->w<640 || o->h<360)return;
    rsx_scene_epoch* e=vk_rt_epoch(depth);
    if(!e)for(unsigned i=0;i<VK_RT_EPOCHS;++i) {
        if(!s_rt_capture.epochs[i])s_rt_capture.epochs[i]=(rsx_scene_epoch*)calloc(1,sizeof *e);
        if(s_rt_capture.epochs[i] && !s_rt_capture.epochs[i]->armed) {e=s_rt_capture.epochs[i];break;}
    }
    if(!e){s_rt_capture.overflow=1;return;}
    rsx_scene_epoch_clear(e,++s_rt_capture.generation,s_rt_capture.frame,0,depth,
        o->w,o->h,o->samples?o->samples:1,value);
}
static void vk_rt_capture_resource(u32 resource)
{
    for(unsigned i=0;i<VK_RT_EPOCHS;++i) {
        rsx_scene_epoch* e=s_rt_capture.epochs[i];
        if(e && (e->depth==resource || rsx_scene_epoch_uses_resource(e,resource)))
            rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_RESOURCE_CHANGED);
    }
}
static void vk_rt_capture_pipeline(u32 pipeline)
{
    for(unsigned i=0;i<VK_RT_EPOCHS;++i) {
        rsx_scene_epoch* e=s_rt_capture.epochs[i];
        if(rsx_scene_epoch_uses_pipeline(e,pipeline))
            rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_RESOURCE_CHANGED);
    }
}
static void vk_rt_capture_draw_impl(u32 pipeline,rsx_topology topology,const void* vertices,
    u32 vertex_count,u32 stride,const u32* indices,u32 index_count)
{
    if(!s_vk.rt_enabled || !pipeline || pipeline>VK_ENG_MAX_PIPE)return;
    const vk_eng_pipe* p=&s_epipe[pipeline-1];
    if(!p->rs.depth_test || !p->rs.depth_write)return;
    const char *hybrid_env=getenv("PS3RECOMP_RSX_HYBRID_PT");
    const int hybrid=hybrid_env && !strcmp(hybrid_env,"1");
    if(hybrid && p->rs.blend_enable)return; // Opaque/cutout BVH; raster retains blended layers.
    rsx_scene_epoch* e=vk_rt_epoch(s_eng_bound.depth);
    if(!e || e->reason!=RSX_SCENE_EPOCH_OK)return;
    if(s_rt_capture.overflow || topology!=RSX_TOPOLOGY_TRIANGLES || !p->scene_vs ||
       !p->scene_ps || (!hybrid && !p->rs.alpha_test_enable && (strstr(p->scene_ps,"discard") || strstr(p->scene_ps,"clip("))) ||
       stride!=p->scene_layout.stride || !s_e2.constant_shadow ||
       !s_e2.have_vp || !s_e2.have_sc || !vk_rt_depth_range(s_e2.vp[4],s_e2.vp[5])) {
        if(!s_rt_capture.rejected_pipeline_logged[pipeline-1]){
            s_rt_capture.rejected_pipeline_logged[pipeline-1]=1;
            VK_LOG("ray capture draw unsupported pipeline=%u frame=%llu topology=%u stride=%u/%u shaders=%u/%u alphaTest=%u viewport=%u scissor=%u depthRange=%.5g/%.5g\n",pipeline,(unsigned long long)s_rt_capture.frame,topology,stride,p->scene_layout.stride,p->scene_vs!=NULL,p->scene_ps!=NULL,p->rs.alpha_test_enable,s_e2.have_vp,s_e2.have_sc,s_e2.vp[4],s_e2.vp[5]);
        }
        rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_UNSUPPORTED);++s_rt_capture.rejected;return;
    }
    size_t total=0;
    for(unsigned i=0;i<VK_RT_EPOCHS;++i)if(s_rt_capture.epochs[i])total+=s_rt_capture.epochs[i]->capacity;
    size_t added=(size_t)vertex_count*stride+(size_t)(indices?index_count:vertex_count)*4u+
        s_e2.vs_bytes+s_e2.ps_bytes+64u;
    size_t projected=e->capacity?e->capacity:65536;
    while(projected<e->used+added && projected<=RSX_SCENE_EPOCH_MAX_BYTES)projected*=2;
    if(projected>RSX_SCENE_EPOCH_MAX_BYTES || total-e->capacity>VK_RT_TOTAL_BYTES-projected) {
        rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_CAPACITY);++s_rt_capture.rejected;return;
    }
    rsx_scene_draw_input in={0};
    in.pipeline=pipeline;in.vertex_count=vertex_count;in.index_count=indices?index_count:0;
    in.vertices=vertices;in.indices=indices;in.layout=p->scene_layout;in.state=p->rs;
    in.vs_constants=s_e2.constant_shadow;in.vs_bytes=s_e2.vs_bytes;
    in.ps_constants=s_e2.constant_shadow+s_g.ps_off;in.ps_bytes=s_e2.ps_bytes;
    /* Opaque AO needs no fragment textures. Live render-target views are not
     * immutable alpha resources and require a separate snapshot implementation. */
    const int needs_alpha=(p->rs.alpha_test_enable && p->rs.alpha_func!=0x207u) ||
        p->rs.blend_enable || (p->rs.antialias_control&0x10u);
    in.texture_mask=needs_alpha?p->scene_alpha_mask:0;
    if(!hybrid && needs_alpha && (p->scene_alpha_contract<0 || !in.texture_mask ||
       (s_e2.tex_mask&in.texture_mask)!=in.texture_mask || s_e2.ps_bytes!=p->scene_alpha_fp_bytes)) {
        if(!s_rt_capture.rejected_pipeline_logged[pipeline-1]){
            s_rt_capture.rejected_pipeline_logged[pipeline-1]=1;
            VK_LOG("ray capture alpha unsupported pipeline=%u contract=%d alpha=%u blend=%u aa=0x%x textureMask=0x%x/0x%x fpBytes=%u/%u\n",pipeline,p->scene_alpha_contract,p->rs.alpha_test_enable,p->rs.blend_enable,p->rs.antialias_control,s_e2.tex_mask,in.texture_mask,s_e2.ps_bytes,p->scene_alpha_fp_bytes);
        }
        rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_UNSUPPORTED);++s_rt_capture.rejected;return;
    }
    if(hybrid)in.texture_mask=s_e2.tex_mask;
    for(unsigned u=0;u<16;++u)if(in.texture_mask&(1u<<u)) {
        const vk_eng_obj* t=vk_eng_get(s_e2.tex[u],VK_ENG_TEXTURE);
        int snapshot=0;
        for(unsigned j=1;j<VK_ENG_MAX_OBJ;++j)
            if((s_eobj[j].kind==VK_ENG_COLOR || s_eobj[j].kind==VK_ENG_DEPTH) &&
               (s_eobj[j].surface==s_e2.tex[u] || s_eobj[j].expanded==s_e2.tex[u]))snapshot=1;
        if(!t || t->surface || t->expanded || t->cube || snapshot) {
            if(hybrid){in.texture_mask&=~(1u<<u);continue;}
            if(!s_rt_capture.rejected_pipeline_logged[pipeline-1]){
                s_rt_capture.rejected_pipeline_logged[pipeline-1]=1;
                VK_LOG("ray capture alpha resource unsupported pipeline=%u unit=%u texture=%u present=%u surface=%u expanded=%u cube=%u snapshot=%d\n",pipeline,u,s_e2.tex[u],t!=NULL,t?t->surface:0,t?t->expanded:0,t?t->cube:0,snapshot);
            }
            rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_UNSUPPORTED);++s_rt_capture.rejected;return;
        }
    }
    const char *authored=getenv("PS3RECOMP_RSX_AUTHORED_LIGHTS");
    int light_contract=rsx_authored__pair(p->scene_vs_sha256,p->scene_ps_sha256);
    if(authored && !strcmp(authored,"1") && light_contract>=0){
        u32 rgb_mask=light_contract==0?7u:light_contract==1?127u:31u,rgb_count=light_contract==0?3u:light_contract==1?7u:5u;
        int ready=(s_e2.tex_mask&rgb_mask)==rgb_mask;
        for(u32 u=0;u<rgb_count&&ready;u++){
            const vk_eng_obj *t=vk_eng_get(s_e2.tex[u],VK_ENG_TEXTURE);
            if(!t || t->surface || t->expanded || t->cube!=(light_contract==1&&u==3))ready=0;
            for(u32 j=1;j<VK_ENG_MAX_OBJ&&ready;j++)if((s_eobj[j].kind==VK_ENG_COLOR||s_eobj[j].kind==VK_ENG_DEPTH)&&(s_eobj[j].surface==s_e2.tex[u]||s_eobj[j].expanded==s_e2.tex[u]))ready=0;
        }
        if(ready)in.texture_mask|=rgb_mask;
    }
    memcpy(in.textures,s_e2.tex,sizeof in.textures);
    memcpy(in.samplers,s_e2.samp,sizeof in.samplers);
    memcpy(in.viewport,s_e2.vp,sizeof in.viewport);memcpy(in.scissor,s_e2.sc,sizeof in.scissor);
    if(rsx_scene_epoch_append(e,s_rt_capture.frame,e->serial+1,&in)) {
        s_rt_capture.copied_bytes+=added;++s_rt_capture.draws;
    } else ++s_rt_capture.rejected;
}
static void vk_rt_capture_draw(u32 pipeline,rsx_topology topology,const void* vertices,
    u32 vertex_count,u32 stride,const u32* indices,u32 index_count)
{
    const u64 started=SDL_GetPerformanceCounter();
    vk_rt_capture_draw_impl(pipeline,topology,vertices,vertex_count,stride,indices,index_count);
    s_rt_capture.copy_ticks+=SDL_GetPerformanceCounter()-started;
}
static void vk_rt_capture_present(void)
{
    ++s_rt_capture.frame;s_rt_capture.overflow=0;
    for(unsigned i=0;i<VK_RT_EPOCHS;++i) {
        rsx_scene_epoch* e=s_rt_capture.epochs[i];
        if(e)rsx_scene_epoch_clear(e,0,s_rt_capture.frame,0,0,0,0,0,0);
    }
}
static void vk_rt_capture_shutdown(void)
{
    for(unsigned i=0;i<VK_RT_EPOCHS;++i) {
        rsx_scene_epoch_destroy(s_rt_capture.epochs[i]);free(s_rt_capture.epochs[i]);
    }
    memset(&s_rt_capture,0,sizeof s_rt_capture);
}

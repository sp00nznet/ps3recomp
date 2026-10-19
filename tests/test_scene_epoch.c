#include "../libs/video/rsx_scene_epoch.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void)
{
    rsx_scene_epoch* e=(rsx_scene_epoch*)calloc(1,sizeof *e);CHECK(e);
    float vertices[12]={1,2,3,1,4,5,6,1,7,8,9,1},vs[2056]={0},ps[4]={0.5f};
    uint32_t indices[3]={2,0,1};rsx_scene_draw_input d={0},out={0};
    d.pipeline=7;d.vertex_count=3;d.vertices=vertices;d.vs_constants=vs;d.vs_bytes=sizeof vs;
    d.ps_constants=ps;d.ps_bytes=sizeof ps;d.layout.stride=16;d.layout.count=1;d.layout.mask=1;
    d.viewport[2]=1280;d.viewport[3]=720;d.viewport[5]=1;
    d.texture_mask=1;d.textures[0]=21;
    CHECK(!rsx_scene_epoch_append(e,1,1,&d));
    rsx_scene_epoch_clear(e,1,1,1,9,1280,720,2,1);
    CHECK(rsx_scene_epoch_append(e,1,2,&d));
    vertices[0]=99;ps[0]=99;vs[0]=99;
    CHECK(rsx_scene_epoch_get(e,0,&out));
    CHECK(((const float*)out.vertices)[0]==1 && ((const float*)out.ps_constants)[0]==0.5f);
    CHECK(((const float*)out.vs_constants)[0]==0 && out.indices[2]==2 && out.index_count==3);
    CHECK(rsx_scene_epoch_uses_resource(e,21) && !rsx_scene_epoch_uses_resource(e,22));
    CHECK(rsx_scene_epoch_uses_pipeline(e,7) && !rsx_scene_epoch_uses_pipeline(e,8));
    CHECK(rsx_scene_epoch_seal(e,1,2));
    CHECK(!rsx_scene_epoch_append(e,1,3,&d) && e->reason==RSX_SCENE_EPOCH_STALE);
    CHECK(!rsx_scene_epoch_get(e,0,&out));
    size_t capacity=e->capacity;
    rsx_scene_epoch_clear(e,2,2,10,9,1280,720,2,1);
    CHECK(e->capacity==capacity && !e->draw_count && !e->used);
    d.indices=indices;d.index_count=3;
    CHECK(rsx_scene_epoch_append(e,2,11,&d));indices[0]=99;
    CHECK(rsx_scene_epoch_get(e,0,&out) && out.indices[0]==2);
    CHECK(!rsx_scene_epoch_seal(e,3,11) && e->reason==RSX_SCENE_EPOCH_STALE);
    rsx_scene_epoch_clear(e,3,3,20,9,1280,720,2,1);
    CHECK(!rsx_scene_epoch_append(e,3,21,&d) && e->reason==RSX_SCENE_EPOCH_UNSUPPORTED);
    d.indices=NULL;d.index_count=0;
    rsx_scene_epoch_clear(e,4,4,30,9,1280,720,2,NAN);
    CHECK(!rsx_scene_epoch_append(e,4,31,&d) && e->reason==RSX_SCENE_EPOCH_NO_CLEAR);
    rsx_scene_epoch_clear(e,5,5,40,9,1280,720,2,1);
    for(unsigned i=0;i<RSX_SCENE_EPOCH_MAX_DRAWS;++i)CHECK(rsx_scene_epoch_append(e,5,41+i,&d));
    CHECK(!rsx_scene_epoch_append(e,5,400,&d) && e->reason==RSX_SCENE_EPOCH_CAPACITY);
    CHECK(e->draw_count==RSX_SCENE_EPOCH_MAX_DRAWS);
    rsx_scene_epoch_clear(e,6,6,500,9,1280,720,2,1);
    CHECK(rsx_scene_epoch_append(e,6,501,&d));
    rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_RESOURCE_CHANGED);
    CHECK(!rsx_scene_epoch_seal(e,6,501) && !rsx_scene_epoch_get(e,0,&out));
    /* Real opaque shaders may have no fragment uniforms. They still have
     * complete vertex inputs and must not poison the scene capture. */
    rsx_scene_epoch_clear(e,7,7,600,9,1280,720,2,1);
    d.ps_bytes=0;d.ps_constants=NULL;
    CHECK(rsx_scene_epoch_append(e,7,601,&d));
    CHECK(rsx_scene_epoch_get(e,0,&out) && out.ps_bytes==0 && out.ps_constants==NULL);
    CHECK(rsx_scene_epoch_seal(e,7,601));
    rsx_scene_epoch_clear(e,8,8,700,9,1280,720,2,1);
    d.state.alpha_test_enable=1;
    CHECK(!rsx_scene_epoch_append(e,8,701,&d) && e->reason==RSX_SCENE_EPOCH_UNSUPPORTED);
    rsx_scene_epoch_clear(e,9,9,800,9,1280,720,2,1);
    d.state.alpha_test_enable=0;d.ps_bytes=16;
    CHECK(!rsx_scene_epoch_append(e,9,801,&d) && e->reason==RSX_SCENE_EPOCH_UNSUPPORTED);
    rsx_scene_epoch_destroy(e);CHECK(!e->bytes && !e->capacity && !e->draw_count);free(e);
    printf("PASS %u immutable scene epoch checks\n",checks);return 0;
}

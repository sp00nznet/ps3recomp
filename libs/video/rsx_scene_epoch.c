#include "rsx_scene_epoch.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

void rsx_scene_epoch_invalidate(rsx_scene_epoch* e,enum rsx_scene_epoch_reason reason)
{ if(e && e->reason==RSX_SCENE_EPOCH_OK)e->reason=reason?reason:RSX_SCENE_EPOCH_UNSUPPORTED; }
void rsx_scene_epoch_clear(rsx_scene_epoch* e,uint64_t generation,uint64_t frame,
    uint64_t serial,uint32_t depth,uint32_t width,uint32_t height,uint32_t samples,float clear)
{
    if(!e)return;
    unsigned char* bytes=e->bytes;size_t capacity=e->capacity;
    memset(e,0,sizeof *e);e->bytes=bytes;e->capacity=capacity;
    e->generation=generation;e->frame=frame;e->serial=serial;e->depth=depth;
    e->width=width;e->height=height;e->samples=samples;
    e->armed=generation && depth && width && height &&
        (samples==1 || samples==2 || samples==4) && isfinite(clear) && clear==1.0f;
    e->reason=e->armed?RSX_SCENE_EPOCH_OK:RSX_SCENE_EPOCH_NO_CLEAR;
}
static size_t align16(size_t n){return (n+15u)&~(size_t)15u;}
int rsx_scene_epoch_append(rsx_scene_epoch* e,uint64_t frame,uint64_t serial,
    const rsx_scene_draw_input* in)
{
    if(!e)return 0;
    if(!e->armed || e->reason!=RSX_SCENE_EPOCH_OK)return 0;
    if(e->sealed || frame!=e->frame || serial<=e->serial) {
        rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_STALE);return 0;
    }
    if(!in || !in->pipeline || !in->vertices || !in->vs_constants || (in->ps_bytes && !in->ps_constants) ||
       !in->vertex_count || in->vertex_count>RSX_SCENE_EPOCH_MAX_VERTICES ||
       in->layout.stride<16 || in->layout.stride>256 || in->layout.stride%16 ||
       in->vs_bytes!=8224 || (in->state.alpha_test_enable && in->ps_bytes<16) || in->ps_bytes>65536 || in->ps_bytes%16 ||
       in->texture_mask>65535 || (!in->indices && in->index_count) ||
       (in->indices && (!in->index_count || in->index_count%3)) ||
       (!in->indices && in->vertex_count%3)) {
        rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_UNSUPPORTED);return 0;
    }
    uint32_t count=in->indices?in->index_count:in->vertex_count;
    if(e->draw_count>=RSX_SCENE_EPOCH_MAX_DRAWS ||
       in->vertex_count>RSX_SCENE_EPOCH_MAX_VERTICES-e->vertex_count ||
       count>RSX_SCENE_EPOCH_MAX_INDICES-e->index_count) {
        rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_CAPACITY);return 0;
    }
    for(unsigned i=0;i<6;++i)if(!isfinite(in->viewport[i])) {
        rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_UNSUPPORTED);return 0;
    }
    for(uint32_t i=0;in->indices && i<count;++i)if(in->indices[i]>=in->vertex_count) {
        rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_UNSUPPORTED);return 0;
    }
    for(unsigned i=0;i<16;++i)if((in->texture_mask&(1u<<i)) && !in->textures[i]) {
        rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_UNSUPPORTED);return 0;
    }
    size_t vb=(size_t)in->vertex_count*in->layout.stride,ib=(size_t)count*4u;
    rsx_scene_draw_snapshot snap={0};snap.draw=*in;
    snap.vertex_offset=align16(e->used);snap.index_offset=align16(snap.vertex_offset+vb);
    snap.vs_offset=align16(snap.index_offset+ib);snap.ps_offset=align16(snap.vs_offset+in->vs_bytes);
    size_t end=align16(snap.ps_offset+in->ps_bytes);
    if(end>RSX_SCENE_EPOCH_MAX_BYTES) {
        rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_CAPACITY);return 0;
    }
    if(end>e->capacity) {
        size_t capacity=e->capacity?e->capacity:65536;
        while(capacity<end)capacity*=2;
        if(capacity>RSX_SCENE_EPOCH_MAX_BYTES)capacity=RSX_SCENE_EPOCH_MAX_BYTES;
        unsigned char* bytes=(unsigned char*)realloc(e->bytes,capacity);
        if(!bytes){rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_ALLOCATION);return 0;}
        e->bytes=bytes;e->capacity=capacity;
    }
    memcpy(e->bytes+snap.vertex_offset,in->vertices,vb);
    if(in->indices)memcpy(e->bytes+snap.index_offset,in->indices,ib);
    else for(uint32_t i=0;i<count;++i)memcpy(e->bytes+snap.index_offset+i*4u,&i,4);
    memcpy(e->bytes+snap.vs_offset,in->vs_constants,in->vs_bytes);
    if(in->ps_bytes)memcpy(e->bytes+snap.ps_offset,in->ps_constants,in->ps_bytes);
    snap.draw.vertices=snap.draw.vs_constants=snap.draw.ps_constants=NULL;
    snap.draw.indices=NULL;snap.draw.index_count=count;
    e->draws[e->draw_count++]=snap;e->used=end;e->vertex_count+=in->vertex_count;
    e->index_count+=count;e->serial=serial;return 1;
}
int rsx_scene_epoch_seal(rsx_scene_epoch* e,uint64_t frame,uint64_t serial)
{
    if(!e || !e->armed || !e->draw_count || e->reason!=RSX_SCENE_EPOCH_OK)return 0;
    if(frame!=e->frame || serial!=e->serial){rsx_scene_epoch_invalidate(e,RSX_SCENE_EPOCH_STALE);return 0;}
    e->sealed=1;return 1;
}
int rsx_scene_epoch_get(const rsx_scene_epoch* e,unsigned index,rsx_scene_draw_input* out)
{
    if(!e || !out || e->reason!=RSX_SCENE_EPOCH_OK || index>=e->draw_count)return 0;
    const rsx_scene_draw_snapshot* s=&e->draws[index];*out=s->draw;
    out->vertices=e->bytes+s->vertex_offset;out->indices=(const uint32_t*)(e->bytes+s->index_offset);
    out->vs_constants=e->bytes+s->vs_offset;
    out->ps_constants=out->ps_bytes?e->bytes+s->ps_offset:NULL;return 1;
}
int rsx_scene_epoch_uses_resource(const rsx_scene_epoch* e,uint32_t texture)
{
    if(!e || !texture)return 0;
    for(unsigned i=0;i<e->draw_count;++i)for(unsigned u=0;u<16;++u)
        if((e->draws[i].draw.texture_mask&(1u<<u)) && e->draws[i].draw.textures[u]==texture)return 1;
    return 0;
}
int rsx_scene_epoch_uses_pipeline(const rsx_scene_epoch* e,uint32_t pipeline)
{
    if(!e || !pipeline)return 0;
    for(unsigned i=0;i<e->draw_count;++i)if(e->draws[i].draw.pipeline==pipeline)return 1;
    return 0;
}
void rsx_scene_epoch_destroy(rsx_scene_epoch* e)
{ if(e){free(e->bytes);memset(e,0,sizeof *e);} }

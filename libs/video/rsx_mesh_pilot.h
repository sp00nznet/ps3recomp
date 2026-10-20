/* Explicit, bounded experimental replacement of exact captured draws.
 * FMP1 is little endian; original bytes and attribute IDs must match in full.
 * No guest memory, animation, collision, shaders or default settings change. */
#ifndef RSX_MESH_PILOT_H
#define RSX_MESH_PILOT_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef struct {
    uint32_t nv, ni, stride, na, attrs[16], out_nv, out_ni;
    unsigned char *original, *replacement;
    size_t original_size, replacement_size;
    unsigned hits;
} rsx_mesh_pilot_record;
#define RSX_MESH_PILOT_MAX_RECORDS 64u
typedef struct { int initialized; unsigned count, gate_poll; rsx_mesh_pilot_record records[RSX_MESH_PILOT_MAX_RECORDS]; } rsx_mesh_pilot_state;
static uint32_t rsx_mesh_pilot_u32(const unsigned char *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void rsx_mesh_pilot_free(rsx_mesh_pilot_state *s) {
    for(unsigned i=0;i<RSX_MESH_PILOT_MAX_RECORDS;i++){free(s->records[i].original);free(s->records[i].replacement);}
    memset(s,0,sizeof *s);
}
static int rsx_mesh_pilot_validate(const unsigned char *p,uint32_t nv,uint32_t ni,uint32_t stride) {
    for(size_t i=0;i<(size_t)nv*stride;i+=4){float f;memcpy(&f,p+i,4);if(!isfinite(f))return 0;}
    for(uint32_t i=0;i<ni;i++)if(rsx_mesh_pilot_u32(p+(size_t)nv*stride+4u*i)>=nv)return 0;
    return 1;
}
static int rsx_mesh_pilot_load(rsx_mesh_pilot_state *s,const char *path) {
    unsigned char h[100];size_t total=0;int ok=0;
    FILE *f=fopen(path,"rb");if(!f)return 0;
    if(fread(h,1,12,f)!=12 || memcmp(h,"FMP1",4) || rsx_mesh_pilot_u32(h+4)!=1)goto done;
    unsigned count=rsx_mesh_pilot_u32(h+8);if(!count || count>RSX_MESH_PILOT_MAX_RECORDS)goto done;
    for(unsigned k=0;k<count;k++){
        if(fread(h,1,96,f)!=96)goto done;
        rsx_mesh_pilot_record *r=&s->records[k];
        r->nv=rsx_mesh_pilot_u32(h);r->ni=rsx_mesh_pilot_u32(h+4);
        r->stride=rsx_mesh_pilot_u32(h+8);r->na=rsx_mesh_pilot_u32(h+12);
        r->out_nv=rsx_mesh_pilot_u32(h+16);r->out_ni=rsx_mesh_pilot_u32(h+20);
        if(rsx_mesh_pilot_u32(h+24)!=4 || rsx_mesh_pilot_u32(h+28)!=0)goto done;
        if(!r->nv || r->nv>65536 || !r->ni || r->ni>196608 || r->ni%3 ||
           !r->na || r->na>16 || r->stride!=r->na*16 ||
           !r->out_nv || r->out_nv>262144 || !r->out_ni || r->out_ni>786432 || r->out_ni%3)goto done;
        uint32_t seen=0;
        for(unsigned a=0;a<16;a++){
            r->attrs[a]=rsx_mesh_pilot_u32(h+32+4*a);
            if(a<r->na){if(r->attrs[a]>15 || (seen&(1u<<r->attrs[a])))goto done;seen|=1u<<r->attrs[a];}
        }
        r->original_size=(size_t)r->nv*r->stride+(size_t)r->ni*4;
        r->replacement_size=(size_t)r->out_nv*r->stride+(size_t)r->out_ni*4;
        total+=r->original_size+r->replacement_size;if(total>32u*1024u*1024u)goto done;
        r->original=(unsigned char*)malloc(r->original_size);
        r->replacement=(unsigned char*)malloc(r->replacement_size);
        if(!r->original || !r->replacement || fread(r->original,1,r->original_size,f)!=r->original_size ||
           fread(r->replacement,1,r->replacement_size,f)!=r->replacement_size ||
           !rsx_mesh_pilot_validate(r->original,r->nv,r->ni,r->stride) ||
           !rsx_mesh_pilot_validate(r->replacement,r->out_nv,r->out_ni,r->stride))goto done;
    }
    if(fgetc(f)!=EOF || ferror(f))goto done;
    s->count=count;ok=1;
done:
    fclose(f);if(!ok)rsx_mesh_pilot_free(s);return ok;
}
static const rsx_mesh_pilot_record *rsx_mesh_pilot_match(rsx_mesh_pilot_state *s,
    uint32_t topology,const void *vertices,uint32_t nv,uint32_t stride,const uint32_t *indices,
    uint32_t ni,const unsigned char *attrs,uint32_t na) {
    if(!s->initialized){
        const char *path=getenv("FOLKLORE_MESH_PILOT");
        const char *trigger=getenv("FOLKLORE_MESH_PILOT_TRIGGER");
        if(path && *path && trigger && *trigger){
            if((s->gate_poll++ & 255u)!=0)return NULL;
            FILE *gate=fopen(trigger,"rb");if(!gate)return NULL;fclose(gate);
        }
        if(path && *path){int ok=rsx_mesh_pilot_load(s,path);fprintf(stderr,"[mesh pilot] load %s records=%u\n",ok?"accepted":"rejected",s->count);}
        s->initialized=1;
    }
    if(topology!=4 || !vertices || !indices || !attrs)return NULL;
    for(unsigned k=0;k<s->count;k++){
        rsx_mesh_pilot_record *r=&s->records[k];
        if(nv!=r->nv || ni!=r->ni || stride!=r->stride || na!=r->na)continue;
        int same=1;for(unsigned a=0;a<na;a++)if(attrs[a]!=r->attrs[a])same=0;
        if(!same || memcmp(vertices,r->original,(size_t)nv*stride) ||
           memcmp(indices,r->original+(size_t)nv*stride,(size_t)ni*4))continue;
        if(!r->hits++)fprintf(stderr,"[mesh pilot] matched record=%u vertices=%u->%u triangles=%u->%u\n",k,nv,r->out_nv,ni/3,r->out_ni/3);
        return r;
    }
    return NULL;
}
#endif

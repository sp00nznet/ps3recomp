#include "rsx_scene_bridge.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

_Static_assert(sizeof(rsx_scene_vertex_output)==192,"scene vertex output layout must be 12 float4 values");

static char* compute_source(const char* vs,const rsx_vertex_layout_plan* l,unsigned count,int full)
{
    static const char entry[]="VSOutput main(VSInput input)";
    static const char replacement[]="VSOutput rsx_scene_vertex(VSInput input)";
    if (!vs || !l || !count || count>262144 || !l->count || l->count>16 || l->stride!=16u*l->count)
        return NULL;
    size_t len=strlen(vs);
    if (len>1024u*1024u || strstr(vs,"Texture") || strstr(vs,"Sampler") || strstr(vs,"rsx_scene_")) return NULL;
    const char* at=strstr(vs,entry);
    if (!at || strstr(at+sizeof(entry)-1,entry)) return NULL;
    unsigned mask=0;
    for(unsigned i=0;i<l->count;++i) {
        if(l->attrs[i]>=16 || (mask&(1u<<l->attrs[i])))return NULL;
        mask|=1u<<l->attrs[i];
    }
    if(mask!=l->mask)return NULL;
    char* out=(char*)malloc(len+4096);if(!out)return NULL;
    size_t prefix=(size_t)(at-vs);
    memcpy(out,vs,prefix);
    memcpy(out+prefix,replacement,sizeof(replacement)-1);
    strcpy(out+prefix+sizeof(replacement)-1,at+sizeof(entry)-1);
    size_t used=strlen(out);
    used+=(size_t)sprintf(out+used,
        "\nRWStructuredBuffer<float4> rsx_scene_in : register(u80);\n"
        "RWStructuredBuffer<float4> rsx_scene_out : register(u81);\n"
        "[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID) {\n"
        "if(id.x>=%u) return; VSInput input=(VSInput)0;\n",count);
    for(unsigned i=0;i<l->count;++i)
        used+=(size_t)sprintf(out+used,"input.a%u=rsx_scene_in[id.x*%u+%u];\n",l->attrs[i],l->count,i);
    if(full) {
        static const char* const fields[]={"pos","col0","col1","fog","t0","t1","t2","t3","t4","t5","t6","t7"};
        used+=(size_t)sprintf(out+used,"VSOutput result=rsx_scene_vertex(input);\n");
        for(unsigned i=0;i<12;++i)
            used+=(size_t)sprintf(out+used,"rsx_scene_out[id.x*12+%u]=result.%s;\n",i,fields[i]);
        strcpy(out+used,"}\n");
    }else strcpy(out+used,"VSOutput result=rsx_scene_vertex(input); rsx_scene_out[id.x]=result.pos; }\n");
    return out;
}

char* rsx_scene_compute_source(const char* vs,const rsx_vertex_layout_plan* l,unsigned count)
{
    return compute_source(vs,l,count,0);
}

char* rsx_scene_compute_outputs_source(const char* vs,const rsx_vertex_layout_plan* l,unsigned count)
{
    return compute_source(vs,l,count,1);
}

int rsx_scene_clip_to_view(const float clip[4],const float p[16],float xyz[3])
{
    if(!clip || !p || !xyz)return 0;
    for(unsigned i=0;i<4;++i)if(!isfinite(clip[i]))return 0;
    for(unsigned i=0;i<16;++i)if(!isfinite(p[i]))return 0;
    /* CACAO's qualified row-major positive-view-Z projection is the only
     * supported layout. Refuse general matrices rather than guessing. */
    if(p[1]!=0 || p[2]!=0 || p[3]!=0 || p[4]!=0 || p[6]!=0 || p[7]!=0 ||
       p[8]!=0 || p[9]!=0 || p[11]!=1 || p[12]!=0 || p[13]!=0 || p[15]!=0 ||
       fabsf(p[0])<1e-12f || fabsf(p[5])<1e-12f || p[14]==0 || fabsf(clip[3])<1e-12f)return 0;
    double ndc[3];
    for(unsigned i=0;i<3;++i)ndc[i]=(double)clip[i]/clip[3];
    double denom=ndc[2]-p[10];
    if(fabs(denom)<1e-12)return 0;
    double z=p[14]/denom;
    float v[3]={(float)(ndc[0]*z/p[0]),(float)(ndc[1]*z/p[5]),(float)z};
    /* Behind-camera vertices are valid geometry for rays and must survive a
     * camera-plane-crossing triangle. Zero homogeneous W was rejected above. */
    if(!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]))return 0;
    memcpy(xyz,v,sizeof v);return 1;
}

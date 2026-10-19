#include "rsx_scene_rgb.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture { float c[16][4],v[12][4],overlay[3]; } fixture;
static int sample(void *ctx,unsigned unit,float u,float v,float lod,float out[4])
{
    fixture *f=ctx;(void)u;(void)lod;
    for(unsigned i=0;i<4;++i) out[i]=unit==0?0.5f:unit==1?(v==0.125f?0.25f:0.75f):1.0f;
    if(unit==2)for(unsigned i=0;i<3;++i)out[i]=f->overlay[i];
    return 1;
}
static void setup(fixture *f,rsx_scene_rgb_input *in)
{
    memset(f,0,sizeof(*f));memset(in,0,sizeof(*in));
    for(unsigned i=0;i<3;++i){f->v[1][i]=(float)(i+1);f->overlay[i]=1;}
    f->v[1][3]=1; f->v[2][0]=f->v[2][1]=1;
    f->v[6][0]=f->v[7][0]=f->v[11][0]=1;
    f->c[2][0]=0.125f;f->c[3][0]=0.25f;
    f->c[14][0]=1;
    in->source_sha256="84d877ddc6083545452948630259c21ca796e7985fcd5e7b5a005666e4a0d634";
    in->fp=(const float (*)[4])f->c;in->fp_bytes=sizeof(f->c);
    in->vertex=(const float (*)[4])f->v;in->vertex_bytes=sizeof(f->v);
    in->sample=sample;in->sample_context=f;
}
static void close3(const float *a,const float *b)
{ for(unsigned i=0;i<3;++i) assert(fabsf(a[i]-b[i])<1e-5f); }
static void invalid_terms(const rsx_scene_rgb_output *o)
{
    assert(!o->valid && isnan(o->source.floor_value) && isnan(o->source.final_scalar));
    for(unsigned j=0;j<2;++j){
        assert(isnan(o->source.fog_weights[j]));
        for(unsigned i=0;i<3;++i) assert(isnan(o->source.lut[j][i]) &&
            isnan(o->source.vertex_weighted[j][i]) && isnan(o->source.fog_colors[j][i]));
    }
    for(unsigned i=0;i<3;++i) assert(isnan(o->source.constant_base[i]) &&
        isnan(o->source.vertex_combined[i]) && isnan(o->source.prefloor[i]) &&
        isnan(o->source.floor_residual[i]) && isnan(o->source.additive[i]) && isnan(o->source.overlay[i]));
}
int main(void)
{
    fixture f;rsx_scene_rgb_input in;rsx_scene_rgb_output o;
    const float one[2]={1,1},zero[3]={0,0,0};float rgb[3],expected[3];
    /* Isolate all five authored contributions independently. */
    for(unsigned term=0;term<5;++term){
        setup(&f,&in);
        unsigned idx=term==0?6:term==1?7:term==2?8:term==3?0:1;
        for(unsigned i=0;i<3;++i) f.c[idx][i]=4;
        if(term==3)f.v[2][3]=0.5f;
        if(term==4)f.v[2][2]=0.25f;
        assert(rsx_scene_rgb_evaluate(&in,&o)==RSX_SCENE_RGB_OK);
        for(unsigned i=0;i<3;++i){
            float base=(float)(i+1),value=base*(term==0?1:term==1?3:term==2?4:term==3?2:1);
            assert(o.source.prefloor[i]==value);
            assert(o.source.lut[0][i]==(term==0?value:0));
            assert(o.source.lut[1][i]==(term==1?value:0));
            assert(o.source.constant_base[i]==(term==2?value:0));
            assert(o.source.vertex_weighted[0][i]==(term==3?value:0));
            assert(o.source.vertex_weighted[1][i]==(term==4?value:0));
            assert(o.source.vertex_combined[i]==(term>=3?value:0));
        }
        assert(rsx_scene_rgb_compose(&o,one,zero,rgb)==RSX_SCENE_RGB_OK);
        assert(!memcmp(rgb,o.authored_rgb,sizeof(rgb)));
    }
    /* Shadows affect only the LUT lamps, preserving vertex/constant/additive. */
    setup(&f,&in);
    for(unsigned i=0;i<3;++i){f.c[6][i]=4;f.c[7][i]=4;f.c[8][i]=2;f.c[10][i]=0.25f;}
    assert(rsx_scene_rgb_evaluate(&in,&o)==0);
    const float shadow[2]={0,0},partial[2]={0.5f,0};
    assert(rsx_scene_rgb_compose(&o,shadow,zero,rgb)==0);
    for(unsigned i=0;i<3;++i)expected[i]=2*((i+1)*2+0.25f);
    close3(rgb,expected);
    for(unsigned lamp=0;lamp<2;++lamp){
        float visibility[2]={0,0};visibility[lamp]=1;
        assert(rsx_scene_rgb_compose(&o,visibility,zero,rgb)==0);
        for(unsigned i=0;i<3;++i)expected[i]=2*((i+1)*(lamp==0?3:5)+0.25f);
        close3(rgb,expected);
    }
    assert(rsx_scene_rgb_compose(&o,partial,zero,rgb)==0);
    for(unsigned i=0;i<3;++i)expected[i]=2*((i+1)*2.5f+0.25f);
    close3(rgb,expected);
    /* No authored LUT light means visibility cannot manufacture a lamp. */
    setup(&f,&in);f.c[10][0]=1;
    assert(rsx_scene_rgb_evaluate(&in,&o)==0);
    assert(rsx_scene_rgb_compose(&o,shadow,zero,rgb)==0);
    close3(rgb,o.authored_rgb);
    /* Independent explicit floor/indirect ordering and two fog blends. */
    setup(&f,&in);f.c[9][0]=0.5f;f.v[2][0]=0.25f;f.v[2][1]=0.75f;f.c[14][0]=0.5f;
    for(unsigned i=0;i<3;++i){f.c[10][i]=0.125f;f.c[12][i]=2;f.c[13][i]=4;f.overlay[i]=2;}
    assert(rsx_scene_rgb_evaluate(&in,&o)==0);
    for(unsigned i=0;i<3;++i){assert(o.source.floor_residual[i]==0.5f);
        assert(o.source.overlay[i]==2 && o.source.additive[i]==0.125f);
        assert(o.source.fog_colors[0][i]==2 && o.source.fog_colors[1][i]==4);}
    assert(o.source.fog_weights[0]==0.25f && o.source.fog_weights[1]==0.75f);
    assert(o.source.floor_value==0.5f && o.source.final_scalar==0.5f);
    assert(rsx_scene_rgb_compose(&o,one,zero,rgb)==0);
    assert(!memcmp(rgb,o.authored_rgb,sizeof(rgb)));
    const float indirect[3]={0.25f,1,2};
    assert(rsx_scene_rgb_compose(&o,one,indirect,rgb)==0);
    for(unsigned i=0;i<3;++i){float a=fmaxf(indirect[i],0.5f)+0.125f;a*=2;
        a=2*0.25f*a+0.75f*2;a=0.75f*a+0.25f*4;expected[i]=0.5f*a;}
    close3(rgb,expected);
    /* Varied finite inputs: full visibility reproduces original bitwise. */
    for(unsigned k=1;k<=1000;++k){
        setup(&f,&in);
        for(unsigned j=0;j<16;++j)for(unsigned i=0;i<3;++i)f.c[j][i]=(float)((k*(j+1)+i)%29)/17;
        f.c[2][0]=0.125f;f.c[3][0]=0.25f;
        for(unsigned i=0;i<4;++i)f.v[2][i]=(float)((k+i)%13)/13;
        assert(rsx_scene_rgb_evaluate(&in,&o)==0);
        assert(rsx_scene_rgb_compose(&o,one,zero,rgb)==0);
        assert(!memcmp(rgb,o.authored_rgb,sizeof(rgb)));
    }
    /* All new source fields invalidate on evaluation failure. */
    in.contract_id=1;assert(rsx_scene_rgb_evaluate(&in,&o)==RSX_SCENE_RGB_UNSUPPORTED);invalid_terms(&o);
    in.contract_id=0;f.c[6][0]=NAN;assert(rsx_scene_rgb_evaluate(&in,&o)==RSX_SCENE_RGB_NUMERIC_UNSUPPORTED);invalid_terms(&o);
    setup(&f,&in);assert(rsx_scene_rgb_evaluate(&in,&o)==0);
    const float bads[4]={NAN,INFINITY,-0.01f,1.01f};
    for(unsigned k=0;k<4;++k)for(unsigned j=0;j<2;++j){
        float vis[2]={1,1};vis[j]=bads[k];
        assert(rsx_scene_rgb_compose(&o,vis,zero,rgb)==RSX_SCENE_RGB_NUMERIC_UNSUPPORTED);
        assert(isnan(rgb[0])&&isnan(rgb[1])&&isnan(rgb[2]));
    }
    for(unsigned j=0;j<3;++j)for(unsigned k=0;k<2;++k){float bad[3]={0,0,0};bad[j]=k?INFINITY:NAN;
        assert(rsx_scene_rgb_compose(&o,one,bad,rgb)==RSX_SCENE_RGB_NUMERIC_UNSUPPORTED);
        assert(isnan(rgb[0])&&isnan(rgb[1])&&isnan(rgb[2]));}
    o.source.overlay[1]=INFINITY;
    assert(rsx_scene_rgb_compose(&o,one,zero,rgb)==RSX_SCENE_RGB_NUMERIC_UNSUPPORTED);
    assert(isnan(rgb[0])&&isnan(rgb[1])&&isnan(rgb[2]));
    o.valid=0;assert(rsx_scene_rgb_compose(&o,one,zero,rgb)==RSX_SCENE_RGB_INVALID);
    assert(isnan(rgb[0])&&isnan(rgb[1])&&isnan(rgb[2]));
    assert(rsx_scene_rgb_compose(NULL,one,zero,rgb)==RSX_SCENE_RGB_INVALID);
    puts("lighting terms: isolated contributions, visibility, fog/floor, 1000 bitwise recompose cases and invalidation passed");
    return 0;
}

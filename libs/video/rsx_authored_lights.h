/* Exact captured source pairs only. Recovery is not a shading implementation.
 * Matrices/projection use the renderer's column-major projection[16] convention;
 * affine outputs are row-major 3x4. Failure never changes *out. */
#ifndef RSX_AUTHORED_LIGHTS_H
#define RSX_AUTHORED_LIGHTS_H
#include "rsx_light_space.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Caller verifies every compact vertex's source position w, not clip.w.
 * Hash qualification alone cannot prove current vertex/skin-weight values. */
#define RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS 0x554E4954u
/* Positive post-skin scale requires per-hit homogeneous light correction. */
#define RSX_AUTHORED_POSITION_POSITIVE_HOMOGENEOUS 0x504F5348u
#define RSX_AUTHORED_CONTRACT_COUNT 3u
enum { RSX_AUTHORED_UNAVAILABLE=0, RSX_AUTHORED_ACTIVE=1, RSX_AUTHORED_DISABLED=2 };
enum { RSX_AUTHORED_TYPE_UNAVAILABLE=0, RSX_AUTHORED_DIRECTIONAL=1,
       RSX_AUTHORED_POINT=2, RSX_AUTHORED_HOMOGENEOUS=3 };
typedef struct rsx_authored_light {
    uint32_t status,type,vc_index,fp_index,has_linear_ramp,source_w_used;
    float authored[4],view[4],rgb[3];
    /* Exact source radial factor: saturate((far-distance)/max(far-near,floor)).
     * distance=1/rsqrt(max(abs(dot(Q,Q)),distance_squared_floor)).
     * Normal dot remains a separate source term, not clamped here. */
    float ramp_near,ramp_far,ramp_denominator_floor,ramp_distance_squared_floor;
} rsx_authored_light;
typedef struct rsx_authored_lights {
    uint32_t contract_id;
    float light_to_view[3][4];
    rsx_authored_light lights[4];
} rsx_authored_lights;
static inline int rsx_authored__hash(const char *a,const char *b)
{
    if(!a)return 0;
    for(unsigned i=0;i<64;++i){char c=a[i];if(c>='A'&&c<='F')c=(char)(c+32);
        if(!c || c!=b[i])return 0;}
    return a[64]==0;
}
static inline int rsx_authored__pair(const char *v,const char *p)
{
    int v0=rsx_authored__hash(v,"4cb35af408f4086bf546b272cd3f5d8bb12e0884e291b7b0350580b10cfb051d") || rsx_authored__hash(v,"45838caa69997a77f4a1a150aab44b7748cb1ff329ebfe72b3adbd2bcee99aad") || rsx_authored__hash(v,"363220f9d386227e399bca756202cb5b216f380cd78acc9adf037058307cc6f1");
    int p0=rsx_authored__hash(p,"84d877ddc6083545452948630259c21ca796e7985fcd5e7b5a005666e4a0d634") || rsx_authored__hash(p,"4b43ead21c9dce22ed005919077251929e3c5efb9890013745ece3afb0cfd4b7");
    int v1=rsx_authored__hash(v,"1770d66bbc62057aac2fb0d267231d6b1e8431c48b8f84eb5af92826b4559190") || rsx_authored__hash(v,"d7a3bee0f35eb3f12b18aa575bd1ec8af521508b2a699976134f0c0cffc2c8b1");
    int p1=rsx_authored__hash(p,"46e66b9feb7f93cff81ce5e9ccd84ef64c9eb35f0e78b44d73b25aedd69d4285") || rsx_authored__hash(p,"6e59f9da9d300213eb50ab8124ca5d5d7e4b6cf177ffc7dbb96c0c544db3d2f6");
    /* TC8 corrected pair regenerated from raw VP9573ca10/FPbeacf82e.
     * Never pair a corrected fragment with a legacy VS that discards TC8. */
    if(rsx_authored__hash(v,"3a2563c31309face3e3d60a14e1c487c5c4f27446a6a4fed6d72567fd6201460") &&
       rsx_authored__hash(p,"7dd199ec45df75fe28c630e58143257605cf911e84c98f0841f31db7d1fc8fe4"))return 1;
    /* Terrain90 exact full VP/FP pair, deformation shared by clip/light paths. */
    if((rsx_authored__hash(v,"cdb25f3e61e58b3c435412a923ae25746243c435e0af23f6ab5821c855d61c57") || rsx_authored__hash(v,"ad7b9336013bac1d4e843567c2e51295cd086064d97a411edf4c30c3b61bcef0")) && (rsx_authored__hash(p,"bc13117035015a01e6b17427f57e095392286b57b4e51e8b4e923d3f16bbc098") || rsx_authored__hash(p,"e31604d3bdfe7359e53f9dea97daa5a5308ba8be5c068bf8c2741fed45812a2e")))return 2;
    return v0&&p0?0:v1&&p1?1:-1;
}
/* Contract0: pipe71 VS:184-214 applies clip rows0..3 and light rows256..258
 * to the SAME deformed r1; :241/:253 use lights13/12 after light-space transform.
 * Pool91: VS:305-338 clips the SAME post-skin P as lights12/13/20/22; no affine
 * transform intervenes. Identity model_to_light refers to post-skin P, never
 * raw vertex attributes. Exact full VS identity protects both deformation facts.
 * VP14 is view/fog; VP244 lobe is outside this bounded four-record contract. */
static inline int rsx_authored_lights_recover(const char *vs_sha256,const char *ps_sha256,
    const float vc[514][4],size_t vc_bytes,const void *fp,size_t fp_bytes,
    const float projection[16],float depth_min,float depth_max,uint32_t position_proof,rsx_authored_lights *out)
{
    int id=rsx_authored__pair(vs_sha256,ps_sha256);
    if((position_proof!=RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS &&
        !(id==1 && position_proof==RSX_AUTHORED_POSITION_POSITIVE_HOMOGENEOUS)) || id<0 || !vc || vc_bytes!=514u*16u || !fp || !out || !projection ||
       fp_bytes!=(id==0?256u:id==1?704u:368u) || !isfinite(depth_min) || !isfinite(depth_max) ||
       depth_min<0 || depth_max>1 || !(depth_max-depth_min>1e-6f))return 0;
    if(!rsx_light_space__finite(projection,16) || projection[0]<=0 || projection[5]<=0 ||
       projection[10]<1 || projection[14]>=0 || projection[11]!=1 || projection[15]!=0)return 0;
    for(unsigned i=0;i<16;++i)if(i!=0&&i!=5&&i!=10&&i!=11&&i!=14&&projection[i]!=0)return 0;
    for(unsigned r=0;r<4;++r)if(!rsx_light_space__finite(vc[r],4))return 0;
    if(!rsx_light_space__finite(vc[512],3)||!rsx_light_space__finite(vc[513],3))return 0;
    /* Pool VS adds clip-only waveform after shared position transform.
     * A nonzero amplitude cannot be represented by this affine recovery. */
    if(id==1 && (!isfinite(vc[467][2]) || vc[467][2]!=0 || vc[464][1]!=1))return 0;
    double effective[4][4];float mv[3][4],ml[3][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    for(unsigned j=0;j<4;++j){
        for(unsigned r=0;r<3;++r)effective[r][j]=(double)vc[r][j]*vc[512][r]+(double)vc[3][j]*vc[513][r];
        effective[2][j]=effective[2][j]*(depth_max-depth_min)+(double)depth_min*vc[3][j];
        effective[3][j]=vc[3][j];
        double predicted=(double)projection[10]*effective[3][j]+(j==3?projection[14]:0);
        double tolerance=1e-5*fmax(1.0,fmax(fabs(predicted),fabs(effective[2][j])));
        if(!isfinite(effective[2][j]) || fabs(effective[2][j]-predicted)>tolerance)return 0;
        if(!rsx_light_space__store(effective[0][j]/projection[0],&mv[0][j]) ||
           !rsx_light_space__store(effective[1][j]/projection[5],&mv[1][j]) ||
           !rsx_light_space__store(effective[3][j],&mv[2][j]))return 0;
    }
    /* Terrain90 also uses rows256..258 after its shared Y deformation. */
    if(id==0 || id==2)memcpy(ml,vc+256,sizeof ml);
    rsx_authored_lights answer;memset(&answer,0,sizeof answer);answer.contract_id=(uint32_t)id;
    if(!rsx_light_space_build(ml,mv,answer.light_to_view))return 0;
    float constants[44][4];memcpy(constants,fp,fp_bytes);
    static const unsigned vi[4]={13,12,20,22};
    static const unsigned pi[3][4]={{6,7,0,0},{16,17,19,18},{11,13,2,1}};
    for(unsigned i=0;i<(id==0?2u:4u);++i){
        rsx_authored_light *l=&answer.lights[i];l->vc_index=vi[i];l->fp_index=pi[id][i];
        if(!rsx_light_space__finite(vc[vi[i]],4)||!rsx_light_space__finite(constants[pi[id][i]],3))return 0;
        memcpy(l->authored,vc[vi[i]],sizeof l->authored);memcpy(l->rgb,constants[pi[id][i]],sizeof l->rgb);
        float source[4];memcpy(source,l->authored,sizeof source);l->source_w_used=i<2;
        if(i>=2)source[3]=1; /* Source explicitly subtracts P, ignores uploaded w. */
        l->type=source[3]==0?RSX_AUTHORED_DIRECTIONAL:source[3]>0?RSX_AUTHORED_POINT:RSX_AUTHORED_HOMOGENEOUS;
        if(!rsx_light_space_homogeneous(answer.light_to_view,source,l->view))return 0;
        l->status=l->rgb[0]==0&&l->rgb[1]==0&&l->rgb[2]==0?RSX_AUTHORED_DISABLED:RSX_AUTHORED_ACTIVE;
        if(i>=2){unsigned ramp=i==2?21u:23u;
            unsigned floor_vc=id==2?466u:464u;
            if(!rsx_light_space__finite(vc[ramp],2)||!isfinite(vc[floor_vc][1])||vc[floor_vc][1]<=0)return 0;
            l->has_linear_ramp=1;l->ramp_near=vc[ramp][0];l->ramp_far=vc[ramp][1];l->ramp_denominator_floor=vc[floor_vc][1];l->ramp_distance_squared_floor=1e-10f;
            /* Inverted/equal authored endpoints are valid source values:
             * max(far-near,floor) defines them, including the captured zero ramp. */
            double denom=fmax((double)l->ramp_far-l->ramp_near,l->ramp_denominator_floor);
            if(!isfinite(denom) || denom>FLT_MAX)return 0;
        }
    }
    *out=answer;return 1;
}
#ifdef __cplusplus
}
#endif
#endif

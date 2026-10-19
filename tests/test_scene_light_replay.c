/* Recover authored homogeneous sources from a qualified captured shader pair.
 * Actual GPU VS replay includes the game's deformation. This validates source
 * coordinates and vectors, not physical radiometry or whole-scene coverage. */
#define RSX_CAPTURE_ALLOW_NONFINITE_ATTRIBUTES 1
#define main rsx_position_replay_fixture_main
#include "test_vulkan_position_replay.c"
#undef main
#include "../libs/video/rsx_ao_projection.h"
#include "../libs/video/rsx_light_space.h"

static u64 light_fingerprint(const char *source) {
    u64 h=14695981039346656037ull;
    for(const unsigned char *s=(const unsigned char*)source;*s;s++){h^=*s;h*=1099511628211ull;}
    return h;
}
static int light_near(float a,float b,float tolerance) {
    return isfinite(a) && isfinite(b) && fabsf(a-b)<=tolerance*fmaxf(1.0f,fmaxf(fabsf(a),fabsf(b)));
}
static int light_normalize(float v[3]) {
    double l=sqrt((double)v[0]*v[0]+(double)v[1]*v[1]+(double)v[2]*v[2]);
    if(!isfinite(l)||l<=0)return 0;
    for(unsigned j=0;j<3;j++)v[j]=(float)(v[j]/l);
    return 1;
}
int main(int argc,char **argv) {
    if(argc!=4){fprintf(stderr,"Usage: captured-geometry qualified-vs qualified-ps\n");return 2;}
    capture c={0};char *vs=NULL,*ps=NULL;rsx_scene_vertex_output *vertices=NULL;
    int result=1,initialized=0;unsigned checked=0;double max_error=0;
    if(!capture_load(&c,argv[1]) || !(vs=shader_load(argv[2])) || !(ps=shader_load(argv[3])))goto done;
    /* Exact CRLF captured source identities, also used by RGB replay fixture. */
    if(strlen(ps)!=4414 || light_fingerprint(ps)!=0x54588ae2925e4660ull ||
       strlen(vs)!=14128 || light_fingerprint(vs)!=0x1725e96003b38bd3ull){
        fprintf(stderr,"Unqualified VS/PS source pair\n");goto done;
    }
    const float (*vc)[4]=(const float (*)[4])c.constants;
    float effective[8],model_view[3][4],light_view[3][4],light[2][4];
    rsx_ao_projection camera;
    if(!rsx_ao_viewport_transform(&vc[512][0],0,1,effective) ||
       !rsx_ao_projection_extract(vc,effective,effective+4,1280.0f/720.0f,&camera)){
        fprintf(stderr,"Current constants do not qualify a common camera\n");goto done;
    }
    for(unsigned k=0;k<4;k++){
        model_view[0][k]=vc[0][k]/camera.matrix[0][0];
        model_view[1][k]=vc[1][k]/camera.matrix[1][1];
        model_view[2][k]=vc[3][k];
        float residual=effective[2]*vc[2][k]+effective[6]*vc[3][k]-camera.depth_a*vc[3][k];
        if(!light_near(residual,k==3?camera.depth_b:0,0.0005f)){
            fprintf(stderr,"Non-affine effective model-to-view reconstruction\n");goto done;
        }
    }
    if(!rsx_light_space_build(vc+256,model_view,light_view) ||
       !rsx_light_space_homogeneous(light_view,vc[13],light[0]) ||
       !rsx_light_space_homogeneous(light_view,vc[12],light[1])){
        fprintf(stderr,"Lighting space is not a qualified rigid camera space\n");goto done;
    }
#ifdef _WIN32
    _putenv_s("PS3RECOMP_VK_GUEST_PROGRAMS","1");
#endif
    SDL_SetMainReady();if(rsx_vulkan_backend_init(1280,720,"Authored light coordinate qualification"))goto done;initialized=1;
    vertices=malloc((size_t)c.h[4]*sizeof *vertices);if(!vertices)goto done;
    if(rsx_vulkan_backend_replay_outputs(vs,&c.layout,c.vertices,c.h[4],c.constants,c.h[30],vertices,c.h[4]))goto done;
    for(u32 i=0;i<c.h[4];i++){
        float p[3];if(!rsx_scene_clip_to_view(vertices[i].pos,&camera.matrix[0][0],p))goto done;
        for(unsigned l=0;l<2;l++){
            const float *source=l?vertices[i].t3:vertices[i].t7;
            float expected[3],actual[3];
            for(unsigned k=0;k<3;k++){
                expected[k]=light[l][k]-p[k]*light[l][3];
                actual[k]=(light_view[k][0]*source[0]+light_view[k][1]*source[1])+light_view[k][2]*source[2];
            }
            if(l && (!light_normalize(expected)||!light_normalize(actual)))goto done;
            for(unsigned k=0;k<3;k++){
                double error=fabs((double)actual[k]-expected[k]);if(error>max_error)max_error=error;
                if(!light_near(actual[k],expected[k],0.0005f)){
                    fprintf(stderr,"Source-vector mismatch vertex=%u light=%u component=%u actual=%.9g expected=%.9g\n",i,l,k,actual[k],expected[k]);goto done;
                }
                checked++;
            }
        }
    }
    printf("{\"scope\":\"captured GPU-deformed vertex light vectors\",\"vertices\":%u,\"checks\":%u,\"max_absolute_error\":%.9g,\"light0_view\":[%.9g,%.9g,%.9g,%.9g],\"light1_view\":[%.9g,%.9g,%.9g,%.9g]}\n",c.h[4],checked,max_error,light[0][0],light[0][1],light[0][2],light[0][3],light[1][0],light[1][1],light[1][2],light[1][3]);
    result=0;
done:
    if(initialized)rsx_vulkan_backend_shutdown();free(c.bytes);free(vs);free(ps);free(vertices);return result;
}

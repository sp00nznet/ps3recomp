#include "../libs/video/rsx_authored_lights.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 0;}}while(0)
static const char *vs[2]={"45838caa69997a77f4a1a150aab44b7748cb1ff329ebfe72b3adbd2bcee99aad","1770d66bbc62057aac2fb0d267231d6b1e8431c48b8f84eb5af92826b4559190"};
static const char *ps[2]={"84d877ddc6083545452948630259c21ca796e7985fcd5e7b5a005666e4a0d634","46e66b9feb7f93cff81ce5e9ccd84ef64c9eb35f0e78b44d73b25aedd69d4285"};
static const char *lfvs[2]={"363220f9d386227e399bca756202cb5b216f380cd78acc9adf037058307cc6f1","d7a3bee0f35eb3f12b18aa575bd1ec8af521508b2a699976134f0c0cffc2c8b1"};
static const char *lfps[2]={"4b43ead21c9dce22ed005919077251929e3c5efb9890013745ece3afb0cfd4b7","6e59f9da9d300213eb50ab8124ca5d5d7e4b6cf177ffc7dbb96c0c544db3d2f6"};
static float vc[514][4],fp[44][4],projection[16];
static void setup(void)
{
    memset(vc,0,sizeof vc);memset(fp,0,sizeof fp);memset(projection,0,sizeof projection);
    projection[0]=2;projection[5]=3;projection[10]=1.01f;projection[11]=1;projection[14]=-.1f;
    vc[0][0]=2;vc[1][1]=3;vc[2][2]=2*projection[10]-1;vc[2][3]=2*projection[14];vc[3][2]=1;
    vc[512][0]=vc[512][1]=1;vc[512][2]=.5f;vc[513][2]=.5f;
    for(unsigned i=0;i<3;++i)vc[256+i][i]=1;
    vc[13][0]=3;vc[12][1]=4;vc[20][0]=5;vc[20][3]=1;vc[22][1]=6;vc[22][3]=1;
    vc[21][0]=30;vc[21][1]=94.5f;vc[464][1]=1;
    fp[6][0]=.25f;fp[7][1]=.5f;fp[17][2]=.75f;fp[19][0]=.85f;
}
static int recover(unsigned id,float n,float f,rsx_authored_lights *out)
{return rsx_authored_lights_recover(vs[id],ps[id],vc,sizeof vc,fp,id?704:256,projection,n,f,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,out);}
static int suite(void)
{
    rsx_authored_lights a,b,saved;memset(&saved,0x5a,sizeof saved);
    for(unsigned id=0;id<2;++id){
        setup();CHECK(recover(id,0,1,&a));CHECK(a.contract_id==id);
        CHECK(a.lights[0].vc_index==13 && a.lights[1].vc_index==12);
        CHECK(a.lights[0].type==RSX_AUTHORED_DIRECTIONAL && a.lights[0].authored[0]==3 && a.lights[0].view[0]==3);
        CHECK(a.lights[1].view[1]==4 && a.light_to_view[0][0]==1);
        CHECK(a.lights[0].fp_index==(id?16u:6u) && a.lights[1].fp_index==(id?17u:7u));
        CHECK(rsx_authored_lights_recover(lfvs[id],lfps[id],vc,sizeof vc,fp,id?704:256,projection,0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&b));
        CHECK(!memcmp(&a,&b,sizeof a));
        /* Exact paired viewport composition: normalized shader Z changes,
         * stored projection and recovered lights remain unchanged. */
        vc[512][2]=.5f/(1.0f-.95f);vc[513][2]=(.5f-.95f)/(1.0f-.95f);
        CHECK(recover(id,.95f,1,&b));CHECK(fabsf(b.light_to_view[2][2]-1)<1e-6f);
        /* Deformation is allowed only through the exact source-pair identity. */
        char changed[65];memcpy(changed,vs[id],65);changed[0]=changed[0]=='0'?'1':'0';b=saved;
        CHECK(!rsx_authored_lights_recover(changed,ps[id],vc,sizeof vc,fp,id?704:256,projection,.95f,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&b));CHECK(!memcmp(&b,&saved,sizeof b));
        b=saved;CHECK(!rsx_authored_lights_recover(vs[id],ps[1-id],vc,sizeof vc,fp,id?704:256,projection,0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&b));CHECK(!memcmp(&b,&saved,sizeof b));
        setup();vc[3][3]=7;vc[0][3]=4;vc[1][3]=15;vc[2][3]=(2*projection[10]-1)*7+2*projection[14];
        CHECK(recover(id,0,1,&b));CHECK(b.light_to_view[0][3]==2 && b.light_to_view[1][3]==5 && b.light_to_view[2][3]==7);
        CHECK(b.lights[0].view[0]==3 && b.lights[0].view[3]==0); /* Direction ignores translation. */
        if(id){CHECK(b.lights[2].view[0]==7 && b.lights[2].view[1]==5 && b.lights[2].view[2]==7);}
        setup();vc[2][0]=.1f;b=saved;CHECK(!recover(id,0,1,&b));CHECK(!memcmp(&b,&saved,sizeof b));
        setup();vc[0][0]*=2;b=saved;CHECK(!recover(id,0,1,&b));CHECK(!memcmp(&b,&saved,sizeof b));
        setup();vc[512][0]=NAN;b=saved;CHECK(!recover(id,0,1,&b));CHECK(!memcmp(&b,&saved,sizeof b));
        setup();b=saved;CHECK(!recover(id,NAN,1,&b) && !recover(id,0,INFINITY,&b) && !recover(id,.95f,.95f,&b));CHECK(!memcmp(&b,&saved,sizeof b));
        CHECK(!rsx_authored_lights_recover(vs[id],ps[id],vc,sizeof vc-16,fp,id?704:256,projection,0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&b));
        CHECK(!rsx_authored_lights_recover(vs[id],ps[id],vc,sizeof vc,fp,id?688:240,projection,0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&b));
        CHECK(!rsx_authored_lights_recover(NULL,ps[id],vc,sizeof vc,fp,id?704:256,projection,0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&b));
    }
    setup();b=saved;CHECK(!rsx_authored_lights_recover(vs[1],ps[1],vc,sizeof vc,fp,704,projection,0,1,0,&b));CHECK(!memcmp(&b,&saved,sizeof b));
    setup();CHECK(rsx_authored_lights_recover(vs[1],ps[1],vc,sizeof vc,fp,704,projection,0,1,RSX_AUTHORED_POSITION_POSITIVE_HOMOGENEOUS,&a));
    CHECK(a.contract_id==1);
    b=saved;CHECK(!rsx_authored_lights_recover(vs[0],ps[0],vc,sizeof vc,fp,256,projection,0,1,RSX_AUTHORED_POSITION_POSITIVE_HOMOGENEOUS,&b));CHECK(!memcmp(&b,&saved,sizeof b));
    setup();vc[464][1]=2;b=saved;CHECK(!recover(1,0,1,&b));CHECK(!memcmp(&b,&saved,sizeof b));
    setup();CHECK(recover(0,0,1,&a));CHECK(a.lights[2].status==RSX_AUTHORED_UNAVAILABLE && a.lights[3].type==RSX_AUTHORED_TYPE_UNAVAILABLE);
    /* Contract0 light space is vc256..258, not identity or raw model space. */
    vc[256][3]=10;CHECK(recover(0,0,1,&a));CHECK(a.light_to_view[0][3]==-10);
    vc[256][0]=0;b=saved;CHECK(!recover(0,0,1,&b));CHECK(!memcmp(&b,&saved,sizeof b));
    setup();CHECK(recover(1,0,1,&a));CHECK(a.lights[0].status==RSX_AUTHORED_DISABLED && a.lights[1].status==RSX_AUTHORED_ACTIVE);
    CHECK(a.lights[2].vc_index==20 && a.lights[2].fp_index==19 && a.lights[2].has_linear_ramp);
    CHECK(a.lights[2].ramp_near==30 && a.lights[2].ramp_far==94.5f && a.lights[2].ramp_denominator_floor==1 && a.lights[2].ramp_distance_squared_floor==1e-10f);
    CHECK(a.lights[3].vc_index==22 && a.lights[3].fp_index==18 && a.lights[3].status==RSX_AUTHORED_DISABLED);
    CHECK(a.lights[3].ramp_near==0 && a.lights[3].ramp_far==0); /* Captured zero ramp is source-valid. */
    vc[20][3]=-7;CHECK(recover(1,0,1,&a));CHECK(a.lights[2].authored[3]==-7 && a.lights[2].view[3]==1 && !a.lights[2].source_w_used);
    vc[13][3]=.25f;CHECK(recover(1,0,1,&a));CHECK(a.lights[0].authored[3]==.25f && a.lights[0].view[3]==.25f);
    vc[14][0]=vc[14][1]=vc[14][2]=NAN;CHECK(recover(1,0,1,&a));for(unsigned i=0;i<4;++i)CHECK(a.lights[i].vc_index!=14);
    vc[21][1]=NAN;b=saved;CHECK(!recover(1,0,1,&b));CHECK(!memcmp(&b,&saved,sizeof b));
    setup();vc[467][2]=.1f;b=saved;CHECK(!recover(1,0,1,&b));CHECK(!memcmp(&b,&saved,sizeof b));
    setup();vc[467][2]=NAN;b=saved;CHECK(!recover(1,0,1,&b));CHECK(!memcmp(&b,&saved,sizeof b));
    setup();fp[19][0]=INFINITY;b=saved;CHECK(!recover(1,0,1,&b));CHECK(!memcmp(&b,&saved,sizeof b));
    /* Terrain90 shared deformation is followed by clip rows0..3 and
     * light rows256..258. It uses its own fragment constants/ramp floor. */
    const char *tv="ad7b9336013bac1d4e843567c2e51295cd086064d97a411edf4c30c3b61bcef0";
    const char *tp="e31604d3bdfe7359e53f9dea97daa5a5308ba8be5c068bf8c2741fed45812a2e";
    setup();vc[466][1]=1;vc[256][3]=10;
    fp[11][0]=.2f;fp[13][1]=.3f;fp[2][2]=.4f;fp[1][0]=.5f;
    CHECK(rsx_authored__pair(tv,tp)==2);
    CHECK(rsx_authored_lights_recover(tv,tp,vc,sizeof vc,fp,368,projection,0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&a));
    CHECK(a.contract_id==2 && a.light_to_view[0][3]==-10);
    CHECK(a.lights[0].fp_index==11 && a.lights[1].fp_index==13 && a.lights[2].fp_index==2 && a.lights[3].fp_index==1);
    CHECK(a.lights[0].rgb[0]==.2f && a.lights[1].rgb[1]==.3f && a.lights[2].rgb[2]==.4f && a.lights[3].rgb[0]==.5f);
    CHECK(a.lights[2].view[0]==-5 && a.lights[2].ramp_denominator_floor==1);
    vc[467][2]=2; /* Shared deformation is allowed, unlike pool clip-only shift. */
    CHECK(rsx_authored_lights_recover(tv,tp,vc,sizeof vc,fp,368,projection,0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&b));
    CHECK(!memcmp(&a,&b,sizeof a));
    vc[466][1]=0;b=saved;
    CHECK(!rsx_authored_lights_recover(tv,tp,vc,sizeof vc,fp,368,projection,0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&b));
    CHECK(!memcmp(&saved,&b,sizeof b));
    vc[466][1]=1;
    CHECK(!rsx_authored_lights_recover(tv,tp,vc,sizeof vc,fp,704,projection,0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&b));
    CHECK(!rsx_authored_lights_recover(tv,tp,vc,sizeof vc,fp,368,projection,0,1,0,&b));
    CHECK(rsx_authored__pair(vs[1],tp)==-1 && rsx_authored__pair(tv,ps[1])==-1);
    return 1;
}
int main(void){if(!suite())return 1;printf("PASS %u authored-light recovery CPU checks; three source families; no GPU\n",checks);return 0;}

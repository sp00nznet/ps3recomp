#include "../libs/video/rsx_scene_bvh.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int near(float a, float b) { return fabsf(a-b) < 0.0001f; }
static double dot(const double a[3], const double b[3])
{ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
/* Independent plane intersection plus Gram-matrix barycentrics. */
static int brute(const float* p, size_t count, const float o[3], const float d[3],
                 float low, float high, size_t* index, double* distance)
{
    int found = 0;
    double best = high;
    for (size_t i = 0; i < count; ++i) {
        const float* a = p + 9*i;
        double e[3], f[3], n[3], delta[3], ray[3], point[3];
        for (unsigned k = 0; k < 3; ++k) {
            e[k] = (double)a[k+3]-a[k]; f[k] = (double)a[k+6]-a[k];
            delta[k] = (double)a[k]-o[k]; ray[k] = d[k];
        }
        n[0] = e[1]*f[2]-e[2]*f[1]; n[1] = e[2]*f[0]-e[0]*f[2];
        n[2] = e[0]*f[1]-e[1]*f[0];
        double denom = dot(n, ray);
        if (denom == 0) continue;
        double t = dot(n, delta)/denom;
        if (t < low || t > best) continue;
        for (unsigned k = 0; k < 3; ++k) point[k] = o[k]+t*d[k]-a[k];
        double ee = dot(e,e), ff = dot(f,f), ef = dot(e,f);
        double gram = ee*ff-ef*ef;
        if (gram <= 0) continue;
        double u = (dot(point,e)*ff-dot(point,f)*ef)/gram;
        double v = (dot(point,f)*ee-dot(point,e)*ef)/gram;
        if (u < 0 || v < 0 || u+v > 1) continue;
        if (!found || t < best || i < *index) { found=1; best=t; *index=i; }
    }
    if (found) *distance=best;
    return found;
}
static uint32_t rng = 173u;
static float random_value(void)
{ rng = rng*1664525u+1013904223u; return (float)(rng >> 8)/16777216.0f; }

static void analytic(void)
{
    float triangles[] = {
        -1,-1,5, 1,-1,5, 0,1,5,
        -1,-1,2, 1,-1,2, 0,1,2,
        0,0,1, 0,0,1, 0,0,1,
        -1,-1,2, 1,-1,2, 0,1,2
    };
    float o[3]={0,0,0}, d[3]={0,0,1};
    rsx_scene_bvh* b=rsx_scene_bvh_create(triangles,4); assert(b);
    rsx_scene_bvh_hit h;
    assert(rsx_scene_bvh_intersect(b,o,d,0,10,&h));
    assert(h.triangle_index==1 && near(h.t,2) && near(h.u,.25f) && near(h.v,.5f));
    assert(near(h.normal[0],0) && near(h.normal[1],0) && near(h.normal[2],1));
    assert(rsx_scene_bvh_intersect(b,o,d,3,10,&h) && h.triangle_index==0 && near(h.t,5));
    assert(rsx_scene_bvh_intersect(b,o,d,2,2,&h) && h.triangle_index==1);
    assert(!rsx_scene_bvh_intersect(b,o,d,0,1.99f,&h));
    assert(!rsx_scene_bvh_intersect(b,o,d,2.01f,4.99f,&h));
    o[2]=10; d[2]=-2;
    assert(rsx_scene_bvh_intersect(b,o,d,0,10,&h) && h.triangle_index==0 && near(h.t,2.5f));
    assert(near(h.normal[2],1));
    o[0]=10; assert(!rsx_scene_bvh_intersect(b,o,d,0,10,&h));
    o[0]=0; d[0]=1; d[2]=0; assert(!rsx_scene_bvh_intersect(b,o,d,0,10,&h));
    o[2]=0; d[0]=0; d[2]=1;
    rsx_scene_bvh_hit before; memset(&before,0x5a,sizeof(before)); h=before;
    assert(!rsx_scene_bvh_intersect(b,o,d,4,3,&h)); assert(!memcmp(&h,&before,sizeof(h)));
    assert(!rsx_scene_bvh_intersect(b,o,d,NAN,10,&h));
    assert(!rsx_scene_bvh_intersect(b,o,d,0,INFINITY,&h));
    assert(!rsx_scene_bvh_intersect(NULL,o,d,0,10,&h));
    assert(!rsx_scene_bvh_intersect(b,NULL,d,0,10,&h));
    assert(!rsx_scene_bvh_intersect(b,o,NULL,0,10,&h));
    assert(!rsx_scene_bvh_intersect(b,o,d,0,10,NULL));
    d[2]=0; assert(!rsx_scene_bvh_intersect(b,o,d,0,10,&h));
    d[2]=NAN; assert(!rsx_scene_bvh_intersect(b,o,d,0,10,&h));
    d[2]=1; o[1]=INFINITY; assert(!rsx_scene_bvh_intersect(b,o,d,0,10,&h));
    assert(!memcmp(&h,&before,sizeof(h)));
    rsx_scene_bvh_destroy(b);
    b=rsx_scene_bvh_create(triangles+18,1); assert(b);
    o[1]=0; assert(!rsx_scene_bvh_intersect(b,o,d,0,10,&h)); rsx_scene_bvh_destroy(b);
    float collinear[]={0,0,2, 1,1,2, 2,2,2};
    b=rsx_scene_bvh_create(collinear,1); assert(b);
    assert(!rsx_scene_bvh_intersect(b,o,d,0,10,&h)); rsx_scene_bvh_destroy(b);
    b=rsx_scene_bvh_create(NULL,0); assert(b);
    assert(!rsx_scene_bvh_intersect(b,o,d,0,10,&h)); rsx_scene_bvh_destroy(b);
    assert(!rsx_scene_bvh_create(NULL,1));
    assert(!rsx_scene_bvh_create(triangles,1000001));
    assert(!rsx_scene_bvh_create(triangles,SIZE_MAX));
    triangles[0]=NAN; assert(!rsx_scene_bvh_create(triangles,1));
    triangles[0]=INFINITY; assert(!rsx_scene_bvh_create(triangles,1));
    rsx_scene_bvh_destroy(NULL);
}
static void many_triangles(void)
{
    enum { COUNT=2048, RAYS=3000 };
    float* p=(float*)malloc(COUNT*9*sizeof(float)); assert(p);
    for (size_t i=0;i<COUNT;++i) {
        /* Arbitrary tilted, separated triangles, shuffled centroid ordering. */
        float x=20*random_value()-10, y=20*random_value()-10, z=20*random_value()+1;
        float* a=p+9*i;
        a[0]=x; a[1]=y; a[2]=z;
        a[3]=x+.1f+random_value(); a[4]=y+.2f*random_value(); a[5]=z+.3f*random_value();
        a[6]=x+.2f*random_value(); a[7]=y+.1f+random_value(); a[8]=z+.3f*random_value();
    }
    rsx_scene_bvh* b=rsx_scene_bvh_create(p,COUNT); assert(b);
    unsigned hits=0, misses=0;
    for (unsigned r=0;r<RAYS;++r) {
        float o[3]={20*random_value()-10,20*random_value()-10,-2};
        float d[3]={random_value()-.5f,random_value()-.5f,1};
        if (r%3==0) {
            const float* a=p+9*(r%COUNT);
            for (unsigned k=0;k<3;++k) d[k]=(a[k]+a[k+3]+a[k+6])/3-o[k];
        }
        size_t index=0; double t=0; rsx_scene_bvh_hit h;
        int expected=brute(p,COUNT,o,d,0,100,&index,&t);
        int actual=rsx_scene_bvh_intersect(b,o,d,0,100,&h);
        assert(actual==expected);
        if (actual) { ++hits; assert(h.triangle_index==index); assert(fabs(h.t-t)<.0001);
            assert(near(h.normal[0]*h.normal[0]+h.normal[1]*h.normal[1]+h.normal[2]*h.normal[2],1));
        } else ++misses;
    }
    assert(hits && misses);
    /* Create owns geometry, so changing/freeing the source is safe. */
    float origin[3]={p[0],p[1],p[2]-1};
    float direction[3]={0,0,1};
    rsx_scene_bvh_hit saved, copied;
    int saved_found=rsx_scene_bvh_intersect(b,origin,direction,0,100,&saved);
    assert(saved_found);
    memset(p,0,COUNT*9*sizeof(float)); free(p);
    assert(rsx_scene_bvh_intersect(b,origin,direction,0,100,&copied));
    assert(copied.triangle_index==saved.triangle_index && copied.t==saved.t);
    rsx_scene_bvh_destroy(b);
    printf("brute-force comparison: %u hits, %u misses, %u rays, %u triangles\n",hits,misses,RAYS,COUNT);
}
static void scale_and_order(void)
{
    /* Force internal splits and place the equal-distance winner far from the
     * initial centroid ordering. Returned identity always refers to input. */
    float p[9*33];
    for (unsigned i=0;i<33;++i) {
        float z=(float)(33-i);
        float a[]={-1,-1,z, 1,-1,z, 0,1,z};
        memcpy(p+9*i,a,sizeof(a));
    }
    p[2]=p[5]=p[8]=1;
    float o[3]={0,0,0}, d[3]={0,0,1};
    rsx_scene_bvh_hit h;
    rsx_scene_bvh* b=rsx_scene_bvh_create(p,33); assert(b);
    assert(rsx_scene_bvh_intersect(b,o,d,0,100,&h) && h.triangle_index==0);
    rsx_scene_bvh_destroy(b);
    /* Very large and small finite geometry is calculated in double precision. */
    const float scales[]={1e-30f,1e30f};
    for (unsigned i=0;i<2;++i) {
        float s=scales[i];
        float a[]={0,0,s, s,0,s, 0,s,s};
        b=rsx_scene_bvh_create(a,1); assert(b);
        o[0]=o[1]=s*.25f; d[2]=s;
        assert(rsx_scene_bvh_intersect(b,o,d,0,2,&h));
        assert(near(h.t,1) && near(h.u,.25f) && near(h.v,.25f) && near(h.normal[2],1));
        rsx_scene_bvh_destroy(b);
    }
    float reversed[]={0,0,1, 0,1,1, 1,0,1};
    o[0]=o[1]=.25f; d[2]=1;
    b=rsx_scene_bvh_create(reversed,1); assert(b);
    assert(rsx_scene_bvh_intersect(b,o,d,0,2,&h) && near(h.normal[2],-1));
    rsx_scene_bvh_destroy(b);
}
int main(void)
{
    analytic(); many_triangles(); scale_and_order();
    puts("scene BVH tests passed");
    return 0;
}

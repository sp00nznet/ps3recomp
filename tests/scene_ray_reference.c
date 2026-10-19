/* CPU reference image for independently comparing Vulkan triangle queries. */
#include "../libs/video/rsx_scene_bvh.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

static int uint_arg(const char* s,unsigned* value,unsigned maximum)
{
    char* end;errno=0;unsigned long v=strtoul(s,&end,10);
    if(errno || !*s || *end || *s=='-' || !v || v>maximum)return 0;
    *value=(unsigned)v;return 1;
}
static int float_arg(const char* s,float* value)
{
    char* end;errno=0;*value=strtof(s,&end);
    return !errno && *s && !*end && isfinite(*value) && *value>0;
}
static float radical_inverse2(uint32_t i)
{
    i=(i<<16)|(i>>16);
    i=((i&0x55555555u)<<1)|((i&0xaaaaaaaau)>>1);
    i=((i&0x33333333u)<<2)|((i&0xccccccccu)>>2);
    i=((i&0x0f0f0f0fu)<<4)|((i&0xf0f0f0f0u)>>4);
    i=((i&0x00ff00ffu)<<8)|((i&0xff00ff00u)>>8);
    return (float)i*2.3283064365386963e-10f;
}
static void cross3(const float a[3],const float b[3],float c[3])
{
    c[0]=a[1]*b[2]-a[2]*b[1];c[1]=a[2]*b[0]-a[0]*b[2];c[2]=a[0]*b[1]-a[1]*b[0];
}
static void normalize3(float v[3])
{
    float length=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    for(unsigned k=0;k<3;++k)v[k]/=length;
}
/* Matches scene_ray_query.comp: camera-facing geometric hemisphere, origin
 * offset along normal, normalized secondary rays, and inclusive [0,distance]. */
static float occlusion(const rsx_scene_bvh* scene,const float point[3],const float normal[3],
                        unsigned samples,float distance,float bias)
{
    float axis[3]={0,0,1},tangent[3],bitangent[3],origin[3];
    if(fabsf(normal[2])>=.999f){axis[1]=1;axis[2]=0;}
    cross3(axis,normal,tangent);normalize3(tangent);cross3(normal,tangent,bitangent);
    for(unsigned k=0;k<3;++k)origin[k]=point[k]+normal[k]*bias;
    unsigned blocked=0;
    for(unsigned i=0;i<samples;++i){
        float u=((float)i+.5f)/(float)samples;
        float phi=6.283185307179586f*radical_inverse2(i);
        float local[3]={sqrtf(u)*cosf(phi),sqrtf(u)*sinf(phi),sqrtf(1-u)},direction[3];
        for(unsigned k=0;k<3;++k)direction[k]=tangent[k]*local[0]+bitangent[k]*local[1]+normal[k]*local[2];
        normalize3(direction);rsx_scene_bvh_hit hit;
        if(rsx_scene_bvh_intersect(scene,origin,direction,0,distance,&hit))++blocked;
    }
    return (float)blocked/(float)samples;
}
int main(int argc,char** argv)
{
    int ao=argc==11 && !strcmp(argv[1],"--ao");
    if(argc!=8 && !ao){fprintf(stderr,"triangles.f32le output-prefix width height sx sy max-distance\n--ao triangles.f32le output-prefix width height sx sy samples distance bias\n");return 2;}
    unsigned shift=ao?1u:0u,w,h,samples=0;
    float sx,sy,far=1e30f,distance=0,bias=0;
    if(!uint_arg(argv[3+shift],&w,4096) || !uint_arg(argv[4+shift],&h,4096) ||
       !float_arg(argv[5+shift],&sx) || !float_arg(argv[6+shift],&sy) || strlen(argv[2+shift])>900)return 2;
    if(ao){
        if(sx>1e6f || sy>1e6f || !uint_arg(argv[8],&samples,256) ||
           !float_arg(argv[9],&distance) || distance>1e6f || !float_arg(argv[10],&bias) || bias>=distance ||
           (uint64_t)w*h*samples>268435456u)return 2;
    }else if(!float_arg(argv[7],&far))return 2;
    uint32_t endian=1;
    if(*(unsigned char*)&endian!=1 || sizeof(float)!=4)return 2;
    const char* prefix=argv[2+shift];
    FILE* f=fopen(argv[1+shift],"rb");if(!f)return 2;
    if(fseek(f,0,SEEK_END)){fclose(f);return 2;}long bytes=ftell(f);
    if(bytes<=0 || bytes%36 || bytes>36000000 || fseek(f,0,SEEK_SET)){fclose(f);return 2;}
    float* triangles=(float*)malloc((size_t)bytes);if(!triangles){fclose(f);return 1;}
    int read_ok=fread(triangles,1,(size_t)bytes,f)==(size_t)bytes;
    if(fclose(f) || !read_ok){free(triangles);return 2;}
    clock_t start=clock();rsx_scene_bvh* scene=rsx_scene_bvh_create(triangles,(size_t)bytes/36);free(triangles);
    if(!scene)return 1;
    float* pixels=(float*)calloc((size_t)w*h,16);unsigned char* rgb=(unsigned char*)malloc((size_t)w*h*3);
    if(!pixels || !rgb){free(pixels);free(rgb);rsx_scene_bvh_destroy(scene);return 1;}
    unsigned hits=0;float origin[3]={0};
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        float dir[3]={(2*((float)x+.5f)/w-1)/sx,(1-2*((float)y+.5f)/h)/sy,1};
        rsx_scene_bvh_hit hit;size_t i=(size_t)y*w+x;
        if(rsx_scene_bvh_intersect(scene,origin,dir,ao?.00001f:.001f,far,&hit)){
            ++hits;memcpy(pixels+i*4,hit.normal,12);pixels[i*4+3]=hit.t;
            if(ao){
                float normal[3],point[3];
                float facing=hit.normal[0]*dir[0]+hit.normal[1]*dir[1]+hit.normal[2]*dir[2];
                for(unsigned k=0;k<3;++k){normal[k]=facing>0?-hit.normal[k]:hit.normal[k];point[k]=dir[k]*hit.t;}
                pixels[i*4]=occlusion(scene,point,normal,samples,distance,bias);
                pixels[i*4+1]=pixels[i*4+2]=0;
                unsigned char shade=(unsigned char)lroundf((1-pixels[i*4])*255);
                memset(rgb+i*3,shade,3);
            }else for(unsigned k=0;k<3;++k)rgb[i*3+k]=(unsigned char)(fmaxf(0,fminf(255,(hit.normal[k]*.5f+.5f)*255)));
        }else memset(rgb+i*3,0,3);
    }
    char path[1024];snprintf(path,sizeof path,ao?"%s.ao.f32le":"%s-rays.f32le",prefix);f=fopen(path,"wb");
    int ok=f!=NULL;
    if(f){
        if(ao){for(size_t i=0;i<(size_t)w*h && ok;++i)ok=fwrite(pixels+i*4,4,1,f)==1;}
        else ok=fwrite(pixels,16,(size_t)w*h,f)==(size_t)w*h;
        if(fclose(f))ok=0;
    }
    snprintf(path,sizeof path,ao?"%s.ao.ppm":"%s-normal.ppm",prefix);f=fopen(path,"wb");
    if(!f)ok=0;else {if(fprintf(f,"P6\n%u %u\n255\n",w,h)<0 || fwrite(rgb,3,(size_t)w*h,f)!=(size_t)w*h)ok=0;if(fclose(f))ok=0;}
    printf("{\"triangles\":%ld,\"rays\":%u,\"hits\":%u,\"cpu_seconds\":%.6f}\n",bytes/36,w*h,hits,(double)(clock()-start)/CLOCKS_PER_SEC);
    free(pixels);free(rgb);rsx_scene_bvh_destroy(scene);return ok?0:1;
}

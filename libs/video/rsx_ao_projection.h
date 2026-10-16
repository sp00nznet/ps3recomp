/* Conservative camera extraction for optional screen-space AO.
 * Unknown vertex programs/projections remain unsupported; never guess a camera.
 * Instruction fields match rsx_vp_decompiler.c's documented NV40 encoding. */
#ifndef RSX_AO_PROJECTION_H
#define RSX_AO_PROJECTION_H
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>

typedef struct rsx_ao_projection {
    float matrix[4][4]; /* CACAO row-major, positive view Z, depth in [0,1]. */
    float depth_a, depth_b;
    float near_z, far_z; /* far_z is infinity for an infinite far plane. */
} rsx_ao_projection;

static inline uint32_t rsx_ao_word(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

/* Accept only four independent DP4(v0, cN) writes to position xyzw.
 * The constants may occur in any instruction order. No side effects accepted.
 * Output indices are committed only on complete success. */
static inline int rsx_ao_position_rows(const void *program, size_t bytes, uint16_t rows[4])
{
    const unsigned char *p = (const unsigned char *)program;
    uint16_t candidate[4];
    unsigned seen = 0;
    if (!p || !rows || bytes != 64) return 0;
    for (unsigned i=0; i<4; ++i, p+=16) {
        uint32_t d0=rsx_ao_word(p), d1=rsx_ao_word(p+4);
        uint32_t d2=rsx_ao_word(p+8), d3=rsx_ao_word(p+12);
        uint32_t s0=((d2>>23)&511)|((d1&255)<<9), s1=(d2>>6)&131071;
        unsigned mask=(d3>>13)&15, component;
        if (((d1>>22)&31)!=7 || (d1>>27)!=0 || ((d1>>8)&15)!=0 ||
            !(d0&(1u<<30)) || ((d0>>15)&63)!=63 ||
            (d0&((7u<<21)|(1u<<26))) ||
            ((d0&(1u<<13)) && ((d0>>10)&7)!=7) ||
            ((d0&(1u<<14)) && (d0&(1u<<29))) ||
            ((d3>>2)&31)!=0 || (d3&2) || ((d3>>17)&15)!=0 ||
            (d3&1)!=(i==3) || !mask || (mask&(mask-1))) return 0;
        /* Non-register source fields must be identity-swizzled and unnegated. */
        if (!((s0==0x1b02 && s1==0x1b03) ||
              (s0==0x1b03 && s1==0x1b02))) return 0;
        component = mask==8 ? 0 : mask==4 ? 1 : mask==2 ? 2 : 3;
        if (seen&(1u<<component)) return 0;
        seen|=1u<<component;
        candidate[component]=(uint16_t)((d1>>12)&1023);
    }
    if (seen!=15 || candidate[0]>508 || candidate[1]!=candidate[0]+1 ||
        candidate[2]!=candidate[0]+2 || candidate[3]!=candidate[0]+3) return 0;
    memcpy(rows,candidate,sizeof(candidate));
    return 1;
}

static inline double rsx_ao_dot3(const float *a, const float *b)
{
    return (double)a[0]*b[0]+(double)a[1]*b[1]+(double)a[2]*b[2];
}

/* Combined world-to-clip rows, followed by the actual shader viewport fixup.
 * Only rigid, centered, forward perspective views are admitted initially.
 * Aspect must describe logical pixels, not expanded MSAA storage dimensions. */
static inline int rsx_ao_projection_extract(const float m[4][4], const float scale[3],
                                   const float offset[3], float aspect,
                                   rsx_ao_projection *out)
{
    rsx_ao_projection candidate;
    double xx,yy,ww,sx,sy,a,b,residual=0;
    if (!m || !scale || !offset || !out || !isfinite(aspect) || aspect<=0) return 0;
    for (unsigned r=0;r<4;++r) for (unsigned c=0;c<4;++c)
        if (!isfinite(m[r][c])) return 0;
    for (unsigned i=0;i<3;++i)
        if (!isfinite(scale[i]) || !isfinite(offset[i])) return 0;
    if (fabs(scale[0]-1)>1e-5 || fabs(scale[1]-1)>1e-5 ||
        fabs(offset[0])>1e-5 || fabs(offset[1])>1e-5 ||
        fabs(scale[2]-.5)>1e-5 || fabs(offset[2]-.5)>1e-5) return 0;
    xx=rsx_ao_dot3(m[0],m[0]); yy=rsx_ao_dot3(m[1],m[1]);
    ww=rsx_ao_dot3(m[3],m[3]);
    if (xx<1e-8 || yy<1e-8 || fabs(ww-1)>1e-3) return 0;
    sx=sqrt(xx); sy=sqrt(yy);
    if (fabs(rsx_ao_dot3(m[0],m[1]))>1e-4*sx*sy ||
        fabs(rsx_ao_dot3(m[0],m[3]))>1e-4*sx ||
        fabs(rsx_ao_dot3(m[1],m[3]))>1e-4*sy ||
        fabs(sy/sx-aspect)>1e-3*aspect) return 0;
    a=rsx_ao_dot3(m[2],m[3])/ww;
    b=m[2][3]-a*m[3][3];
    for (unsigned i=0;i<3;++i) {
        double e=m[2][i]-a*m[3][i]; residual+=e*e;
    }
    if (residual>1e-10 || a<1 || b>=-1e-8 || sx>1000 || sy>1000) return 0;
    memset(&candidate,0,sizeof(candidate));
    candidate.depth_a=(float)(.5*a+.5);
    candidate.depth_b=(float)(.5*b);
    candidate.near_z=(float)(-b/(a+1));
    candidate.far_z=a==1 ? INFINITY : (float)(-b/(a-1));
    candidate.matrix[0][0]=(float)sx;
    candidate.matrix[1][1]=(float)sy;
    candidate.matrix[2][2]=candidate.depth_a;
    candidate.matrix[2][3]=1;
    candidate.matrix[3][2]=candidate.depth_b;
    if (!isfinite(candidate.depth_a) || !isfinite(candidate.depth_b) ||
        !isfinite(candidate.near_z) || candidate.near_z<=0 ||
        candidate.far_z<=candidate.near_z) return 0;
    *out=candidate;
    return 1;
}
#endif

/* AO metadata follows a depth image's actual clear/write lifetime. */
#ifndef RSX_AO_DEPTH_STATE_H
#define RSX_AO_DEPTH_STATE_H
#include "rsx_ao_projection.h"
typedef struct rsx_ao_depth_state {
    rsx_ao_projection projection;
    uint64_t serial;
    uint32_t frame, width, height, draws, accepted, unknown, conflicts;
    int armed, blocked;
} rsx_ao_depth_state;

static inline void rsx_ao_depth_clear(rsx_ao_depth_state *s, uint32_t frame,
                                    float depth, uint64_t serial)
{
    memset(s,0,sizeof(*s));s->frame=frame;s->serial=serial;
    /* A forward-depth AO view needs a known far-plane background. */
    s->armed=isfinite(depth) && depth==1.0f;
    s->blocked=!s->armed;
}

static inline void rsx_ao_depth_write(rsx_ao_depth_state *s, uint32_t frame,
    uint64_t serial, uint32_t width, uint32_t height, const rsx_ao_projection *p)
{
    s->serial=serial;++s->draws;
    if (!s->armed || frame!=s->frame || !width || !height || !p) {
        ++s->unknown;s->blocked=1;return;
    }
    if (!s->accepted) {
        s->projection=*p;s->width=width;s->height=height;
    } else {
        if (s->width!=width || s->height!=height) {
            ++s->conflicts;s->blocked=1;return;
        }
        /* Compare projection coefficients, not unstable derived far distances.
         * Combined matrices can differ in world/model translation/rotation. */
        for (unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) {
            const double a=s->projection.matrix[r][c],b=p->matrix[r][c];
            const double tolerance=(r==3 && c==2 ? 5e-4 : 1e-5)*fmax(1.0,fabs(a));
            if (!isfinite(b) || fabs(a-b)>tolerance) {
                ++s->conflicts;s->blocked=1;return;
            }
        }
    }
    ++s->accepted;
}

static inline int rsx_ao_depth_usable(const rsx_ao_depth_state *s,
                                    uint32_t frame, uint64_t serial)
{
    return s->armed && !s->blocked && s->draws && s->draws==s->accepted &&
           s->frame==frame && s->serial==serial;
}
#endif

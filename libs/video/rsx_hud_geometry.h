/* Optional post-viewport HUD geometry. No draw classification lives here. */
#ifndef RSX_HUD_GEOMETRY_H
#define RSX_HUD_GEOMETRY_H

#include <float.h>
#include <math.h>
#include <stdint.h>

typedef struct rsx_hud_rect { uint32_t x, y, w, h; } rsx_hud_rect;

/* Guest viewport/scissor coverage, before optional backend render scaling. */
static inline int rsx_hud_covers_target(uint32_t w,uint32_t h,
                                        rsx_hud_rect viewport,rsx_hud_rect scissor,
                                        float sx,float sy,float tx,float ty)
{
    return w && h && viewport.x==0 && viewport.y==0 && viewport.w>=w && viewport.h>=h &&
        scissor.x==0 && scissor.y==0 && scissor.w>=w && scissor.h>=h &&
        fabsf(sx*2.0f-w)<0.01f && fabsf(fabsf(sy)*2.0f-h)<0.01f &&
        fabsf(tx-w*0.5f)<0.01f && fabsf(ty-h*0.5f)<0.01f;
}

/* Clip-space anchor: 0 first edge, 1 last edge, 2 center. */
static inline int rsx_hud_anchor_clip(double low, double high, int vertical)
{
    const double sum = low + high;
    if (fabs(sum) < 0.70 || (!vertical && high - low > 1.50)) return 2;
    return vertical ? sum < 0.0 : sum >= 0.0;
}

/* A sliced corner HUD must use one edge anchor even when a short slice's
 * center falls inside the central overlay region. The captured status-bar
 * shader also contains transparent vertices crossing the screen center.
 * Preserve center anchors for central overlays and genuinely centered quads. */
static inline int rsx_hud_horizontal_anchor(const double low[2],const double high[2],int status_marker)
{
    const int corner_band=low[1]>=0.5 || high[1]<=-0.5;
    if(status_marker || (corner_band && (low[0]>=0.0 || high[0]<=0.0)))
        return low[0]+high[0]>=0.0;
    return rsx_hud_anchor_clip(low[0],high[0],0);
}

/* Accept one canonical decimal integer in [50,100]. The caller owns defaults. */
static inline int rsx_hud_parse_percent(const char *text, unsigned *out)
{
    unsigned value = 0;
    const char *p = text;
    if (!p || !*p || !out || (*p == '0' && p[1])) return -1;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
        value = value * 10u + (unsigned)(*p - '0');
        if (value > 100u) return -1;
    }
    if (value < 50u) return -1;
    *out = value;
    return 0;
}

static inline int rsx_hud_parse_scale(const char *text, float *out)
{
    unsigned percent;
    if (!out || rsx_hud_parse_percent(text, &percent)) return -1;
    *out = (float)percent / 100.0f;
    return 0;
}

/* xf is (scale x,y,z,w; offset x,y,z,w). Compose a clip-space scale
 * about the selected corner after the existing viewport transform. */
static inline int rsx_hud_compose_xf_aspect(const float in[8], unsigned percent,
                                            float horizontal_factor,
                                            int right, int bottom, float out[8])
{
    float result[8];
    const double s = (double)percent / 100.0;
    const double sx = s * (double)horizontal_factor;
    const double ax = right == 2 ? 0.0 : right ? 1.0 : -1.0;
    const double ay = bottom == 2 ? 0.0 : bottom ? -1.0 : 1.0;
    if (!in || !out || percent < 50u || percent > 100u ||
        !isfinite(horizontal_factor) || horizontal_factor <= 0.0f || horizontal_factor > 1.0f ||
        (right < 0 || right > 2) || (bottom < 0 || bottom > 2)) return -1;
    for (unsigned i = 0; i < 8; ++i)
        if (!isfinite(in[i])) return -1;
    const double xscale = sx * (double)in[0];
    const double yscale = s * (double)in[1];
    const double xoffset = sx * (double)in[4] + (1.0 - sx) * ax;
    const double yoffset = s * (double)in[5] + (1.0 - s) * ay;
    if (!isfinite(xscale) || !isfinite(yscale) ||
        !isfinite(xoffset) || !isfinite(yoffset) ||
        fabs(xscale) > FLT_MAX || fabs(yscale) > FLT_MAX ||
        fabs(xoffset) > FLT_MAX || fabs(yoffset) > FLT_MAX) return -1;
    for (unsigned i = 0; i < 8; ++i) result[i] = in[i];
    result[0] = (float)xscale; result[1] = (float)yscale;
    result[4] = (float)xoffset; result[5] = (float)yoffset;
    for (unsigned i = 0; i < 8; ++i) out[i] = result[i];
    return 0;
}

static inline int rsx_hud_compose_xf(const float in[8], unsigned percent,
                                     int right, int bottom, float out[8])
{
    return rsx_hud_compose_xf_aspect(in, percent, 1.0f, right, bottom, out);
}

/* Transform pixel-space scissor edges about the corresponding target corner.
 * Round outward, then clip before integer conversion. */
static inline int rsx_hud_scale_scissor_aspect(rsx_hud_rect in,
                                               uint32_t target_w, uint32_t target_h,
                                               unsigned percent, float horizontal_factor,
                                               int right, int bottom, rsx_hud_rect *out)
{
    const double s = (double)percent / 100.0;
    const double sx = s * (double)horizontal_factor;
    double ax, ay, lo_x, hi_x, lo_y, hi_y;
    rsx_hud_rect result;
    if (!out || !target_w || !target_h || !in.w || !in.h ||
        percent < 50u || percent > 100u ||
        !isfinite(horizontal_factor) || horizontal_factor <= 0.0f || horizontal_factor > 1.0f ||
        (right < 0 || right > 2) || (bottom < 0 || bottom > 2)) return -1;
    ax = right == 2 ? (double)target_w * 0.5 : right ? (double)target_w : 0.0;
    ay = bottom == 2 ? (double)target_h * 0.5 : bottom ? (double)target_h : 0.0;
    lo_x = floor(ax + sx * ((double)in.x - ax));
    hi_x = ceil(ax + sx * ((double)((uint64_t)in.x + in.w) - ax));
    lo_y = floor(ay + s * ((double)in.y - ay));
    hi_y = ceil(ay + s * ((double)((uint64_t)in.y + in.h) - ay));
    if (!isfinite(lo_x) || !isfinite(hi_x) || !isfinite(lo_y) || !isfinite(hi_y)) return -1;
    if (lo_x < 0.0) lo_x = 0.0;
    if (hi_x < 0.0) hi_x = 0.0;
    if (lo_y < 0.0) lo_y = 0.0;
    if (hi_y < 0.0) hi_y = 0.0;
    if (lo_x > target_w) lo_x = target_w;
    if (hi_x > target_w) hi_x = target_w;
    if (lo_y > target_h) lo_y = target_h;
    if (hi_y > target_h) hi_y = target_h;
    result.x = (uint32_t)lo_x; result.y = (uint32_t)lo_y;
    result.w = (uint32_t)hi_x - result.x;
    result.h = (uint32_t)hi_y - result.y;
    *out = result;
    return 0;
}

static inline int rsx_hud_scale_scissor(rsx_hud_rect in,
                                        uint32_t target_w, uint32_t target_h,
                                        unsigned percent, int right, int bottom,
                                        rsx_hud_rect *out)
{
    return rsx_hud_scale_scissor_aspect(in, target_w, target_h, percent,
                                        1.0f, right, bottom, out);
}

#endif

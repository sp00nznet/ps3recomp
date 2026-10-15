/* Narrow, exact NV3089 TRUNCATE contract, shared with the host-only probe.
 * RPCS3 4d5def5880217135554ed434cd5055cceebe9cc2 gcm_enums.h defines
 * C2FC=SET_COLOR_CONVERSION, 1=TRUNCATE, source ARGB8888=3/RGB565=7,
 * destination ARGB8888=10/RGB565=4, surface2d=313371C3, SRCCOPY=3.
 * Same-format truncation discards no component bits, so copying is exact.
 * Dither, subtract-truncate and actual format conversions are not emulated. */
#ifndef RSX_NV3089_TRUNCATE_H
#define RSX_NV3089_TRUNCATE_H
#include <stdint.h>

enum rsx_nv3089_contract {
    RSX_NV3089_EXACT = 0, RSX_NV3089_EMPTY, RSX_NV3089_MODE,
    RSX_NV3089_CONTEXT, RSX_NV3089_FORMAT, RSX_NV3089_OPERATION,
    RSX_NV3089_SAMPLING, RSX_NV3089_CLIPPING, RSX_NV3089_PITCH,
    RSX_NV3089_BOUNDS, RSX_NV3089_OVERLAP, RSX_NV3089_DMA
};
typedef struct rsx_nv3089_truncate {
    uint32_t mode, context, source_format, destination_format, operation;
    uint32_t ds_dx, dt_dy, input_uv, input_format, input_size;
    uint32_t output_point, output_size, clip_point, clip_size, destination_pitch;
} rsx_nv3089_truncate;

static inline enum rsx_nv3089_contract rsx_nv3089_truncate_check(
    const rsx_nv3089_truncate* s, unsigned* bytes_per_pixel)
{
    const uint32_t w=s->output_size&65535u,h=s->output_size>>16;
    const uint32_t x=s->output_point&65535u,y=s->output_point>>16;
    const uint32_t cx=s->clip_point&65535u,cy=s->clip_point>>16;
    const uint32_t cw=s->clip_size&65535u,ch=s->clip_size>>16;
    const uint32_t origin=(s->input_format>>16)&255u;
    unsigned bpp;
    if(s->mode!=1)return RSX_NV3089_MODE;
    if(s->context!=0x313371C3u)return RSX_NV3089_CONTEXT;
    if(s->source_format==3 && s->destination_format==10)bpp=4;
    else if(s->source_format==7 && s->destination_format==4)bpp=2;
    else return RSX_NV3089_FORMAT;
    if(s->operation!=3)return RSX_NV3089_OPERATION;
    if(s->ds_dx!=0x100000u || s->dt_dy!=0x100000u || s->input_uv ||
       (origin!=1 && origin!=2) || (s->input_format&0xff000000u))return RSX_NV3089_SAMPLING;
    if(!w || !h || !cw || !ch)return RSX_NV3089_EMPTY;
    if(w>(s->input_size&65535u) || h>(s->input_size>>16))return RSX_NV3089_BOUNDS;
    /* The GPU mirror currently copies the output rectangle before the CPU
     * clip. Restrict this path to a clip that contains that entire rectangle. */
    if(x<cx || y<cy || (uint64_t)x+w>(uint64_t)cx+cw ||
       (uint64_t)y+h>(uint64_t)cy+ch)return RSX_NV3089_CLIPPING;
    if((uint64_t)w*bpp>(s->input_format&65535u) ||
       (uint64_t)(x+w)*bpp>s->destination_pitch)return RSX_NV3089_PITCH;
    *bytes_per_pixel=bpp;
    return RSX_NV3089_EXACT;
}

static inline enum rsx_nv3089_contract rsx_nv3089_truncate_spans(
    const rsx_nv3089_truncate* s,unsigned bpp,uint32_t src,uint32_t dst,uint64_t limit)
{
    const uint64_t w=s->output_size&65535u,h=s->output_size>>16;
    const uint64_t x=s->output_point&65535u,y=s->output_point>>16;
    const uint64_t first_dst=(uint64_t)dst+y*s->destination_pitch+x*bpp;
    const uint64_t end_src=(uint64_t)src+(h-1)*(s->input_format&65535u)+w*bpp;
    const uint64_t end_dst=(uint64_t)dst+(y+h-1)*s->destination_pitch+(x+w)*bpp;
    if(!w || !h || src<65536u || first_dst<65536u || end_src>limit || end_dst>limit)
        return RSX_NV3089_BOUNDS;
    if((uint64_t)src==first_dst && (s->input_format&65535u)==s->destination_pitch)
        return RSX_NV3089_EMPTY;
    if((uint64_t)src<end_dst && first_dst<end_src)
        return RSX_NV3089_OVERLAP;
    return RSX_NV3089_EXACT;
}
#endif

/* Bounded guest payload span for optional FTX1 scene capture. This helper is
 * independent of the draw engine so its overflow checks can be tested alone. */
#ifndef RSX_SCENE_TEXTURE_LAYOUT_H
#define RSX_SCENE_TEXTURE_LAYOUT_H

#include "rsx_texture_layout.h"

static inline u32 rsx_scene_texture_capture_span(u32 format,u32 width,u32 height,
                                                  u32 mipmaps,u32 pitch,int cubemap)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n=rsx_texture_mip_chain(format,width,height,mipmaps,pitch,lv);
    if(!n)return 0;
    u64 end=0;
    for(u32 m=0;m<n;++m){
        const u64 bytes=(u64)lv[m].tl.row_bytes*lv[m].tl.rows;
        if(!bytes || bytes!=lv[m].tl.face_bytes || lv[m].offset!=end)return 0;
        end+=bytes;
        if(end>16u*1024u*1024u)return 0;
    }
    const u64 stride=cubemap?(end+127u)&~127ull:end;
    const u64 span=stride*(cubemap?6u:1u);
    if(!span || span>16u*1024u*1024u ||
       (cubemap && stride!=rsx_texture_cube_face_stride(format,width,height,mipmaps,pitch)))return 0;
    return (u32)span;
}

#endif

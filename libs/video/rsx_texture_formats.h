/*
 * ps3recomp - RSX Texture Format Mapping
 *
 * Maps RSX NV40-style texture formats to DXGI formats for D3D12.
 * Reference: RPCS3 rpcs3/Emu/RSX/gcm_enums.h and rsx_utils.h
 */

#ifndef PS3RECOMP_RSX_TEXTURE_FORMATS_H
#define PS3RECOMP_RSX_TEXTURE_FORMATS_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RSX texture format values (from NV4097_SET_TEXTURE_FORMAT, bits 8-15) */
#define RSX_TEXTURE_B8               0x81
#define RSX_TEXTURE_A1R5G5B5         0x82
#define RSX_TEXTURE_A4R4G4B4         0x83
#define RSX_TEXTURE_R5G6B5           0x84
#define RSX_TEXTURE_A8R8G8B8         0x85
#define RSX_TEXTURE_COMPRESSED_DXT1  0x86
#define RSX_TEXTURE_COMPRESSED_DXT23 0x87
#define RSX_TEXTURE_COMPRESSED_DXT45 0x88
#define RSX_TEXTURE_G8B8             0x8B
#define RSX_TEXTURE_R6G5B5           0x8F
#define RSX_TEXTURE_DEPTH24_D8       0x90
#define RSX_TEXTURE_DEPTH24_D8_FLOAT 0x91
#define RSX_TEXTURE_DEPTH16          0x92
#define RSX_TEXTURE_DEPTH16_FLOAT    0x93
#define RSX_TEXTURE_X16              0x94  /* float16 single channel */
#define RSX_TEXTURE_Y16_X16          0x95  /* float16 two channels */
#define RSX_TEXTURE_R5G5B5A1         0x97
#define RSX_TEXTURE_W16_Z16_Y16_X16_FLOAT 0x9A
#define RSX_TEXTURE_W32_Z32_Y32_X32_FLOAT 0x9B
#define RSX_TEXTURE_X32_FLOAT        0x9C
#define RSX_TEXTURE_D1R5G5B5         0x9D
#define RSX_TEXTURE_D8R8G8B8         0x9E
#define RSX_TEXTURE_Y16_X16_FLOAT    0x9F
#define RSX_TEXTURE_COMPRESSED_B8R8_G8R8 0xAD
#define RSX_TEXTURE_COMPRESSED_R8B8_R8G8 0xAE

/* Extract the base format from the full texture format word.
 * The format field is in bits 8-15 of the NV4097_SET_TEXTURE_FORMAT register. */
static inline u32 rsx_texture_get_format(u32 format_word)
{
    return (format_word >> 8) & 0xFF;
}

/* Check if format is swizzled (bit 5 of format_word) */
static inline int rsx_texture_is_swizzled(u32 format_word)
{
    return (format_word & 0x20) != 0;
}

/* Get bits per pixel for an RSX texture format */
static inline u32 rsx_texture_bpp(u32 format)
{
    switch (format) {
    case RSX_TEXTURE_B8:             return 8;
    case RSX_TEXTURE_A1R5G5B5:      return 16;
    case RSX_TEXTURE_A4R4G4B4:      return 16;
    case RSX_TEXTURE_R5G6B5:        return 16;
    case RSX_TEXTURE_A8R8G8B8:      return 32;
    case RSX_TEXTURE_G8B8:          return 16;
    case RSX_TEXTURE_X16:           return 16;
    case RSX_TEXTURE_Y16_X16:       return 32;
    case RSX_TEXTURE_Y16_X16_FLOAT: return 32;
    case RSX_TEXTURE_X32_FLOAT:     return 32;
    case RSX_TEXTURE_W16_Z16_Y16_X16_FLOAT: return 64;
    case RSX_TEXTURE_W32_Z32_Y32_X32_FLOAT: return 128;
    case RSX_TEXTURE_DEPTH16:       return 16;
    case RSX_TEXTURE_DEPTH16_FLOAT: return 16;
    case RSX_TEXTURE_DEPTH24_D8:    return 32;
    case RSX_TEXTURE_DEPTH24_D8_FLOAT: return 32;
    case RSX_TEXTURE_COMPRESSED_DXT1:  return 4; /* per texel average */
    case RSX_TEXTURE_COMPRESSED_DXT23: return 8;
    case RSX_TEXTURE_COMPRESSED_DXT45: return 8;
    default:                         return 32;
    }
}

/* DXGI format constants (subset needed for texture mapping) */
#define DXGI_FORMAT_R8_UNORM_TEX         61
#define DXGI_FORMAT_R8G8_UNORM_TEX       49
#define DXGI_FORMAT_B8G8R8A8_UNORM       87
#define DXGI_FORMAT_B5G5R5A1_UNORM       86
#define DXGI_FORMAT_B5G6R5_UNORM         85
#define DXGI_FORMAT_B4G4R4A4_UNORM       115
#define DXGI_FORMAT_R16_FLOAT_TEX        54
#define DXGI_FORMAT_R16G16_FLOAT_TEX     34
#define DXGI_FORMAT_R16G16B16A16_FLOAT_TEX 10
#define DXGI_FORMAT_R32_FLOAT_TEX        41
#define DXGI_FORMAT_R32G32B32A32_FLOAT_TEX 2
#define DXGI_FORMAT_BC1_UNORM            71  /* DXT1 */
#define DXGI_FORMAT_BC2_UNORM            74  /* DXT3 */
#define DXGI_FORMAT_BC3_UNORM            77  /* DXT5 */
#define DXGI_FORMAT_D24_UNORM_S8_UINT    45
#define DXGI_FORMAT_D16_UNORM            55

/* Map RSX texture format to DXGI format.
 * Returns 0 (DXGI_FORMAT_UNKNOWN) for unsupported formats. */
static inline u32 rsx_to_dxgi_texture_format(u32 rsx_format)
{
    switch (rsx_format) {
    case RSX_TEXTURE_B8:             return DXGI_FORMAT_R8_UNORM_TEX;
    case RSX_TEXTURE_G8B8:           return DXGI_FORMAT_R8G8_UNORM_TEX;
    case RSX_TEXTURE_A8R8G8B8:       return DXGI_FORMAT_B8G8R8A8_UNORM;
    case RSX_TEXTURE_D8R8G8B8:       return DXGI_FORMAT_B8G8R8A8_UNORM;
    case RSX_TEXTURE_A1R5G5B5:       return DXGI_FORMAT_B5G5R5A1_UNORM;
    case RSX_TEXTURE_R5G5B5A1:       return DXGI_FORMAT_B5G5R5A1_UNORM;
    case RSX_TEXTURE_R5G6B5:         return DXGI_FORMAT_B5G6R5_UNORM;
    case RSX_TEXTURE_A4R4G4B4:       return DXGI_FORMAT_B4G4R4A4_UNORM;
    case RSX_TEXTURE_X16:            return DXGI_FORMAT_R16_FLOAT_TEX;
    case RSX_TEXTURE_Y16_X16:        return DXGI_FORMAT_R16G16_FLOAT_TEX;
    case RSX_TEXTURE_Y16_X16_FLOAT:  return DXGI_FORMAT_R16G16_FLOAT_TEX;
    case RSX_TEXTURE_W16_Z16_Y16_X16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT_TEX;
    case RSX_TEXTURE_X32_FLOAT:      return DXGI_FORMAT_R32_FLOAT_TEX;
    case RSX_TEXTURE_W32_Z32_Y32_X32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT_TEX;
    case RSX_TEXTURE_COMPRESSED_DXT1:  return DXGI_FORMAT_BC1_UNORM;
    case RSX_TEXTURE_COMPRESSED_DXT23: return DXGI_FORMAT_BC2_UNORM;
    case RSX_TEXTURE_COMPRESSED_DXT45: return DXGI_FORMAT_BC3_UNORM;
    case RSX_TEXTURE_DEPTH16:        return DXGI_FORMAT_D16_UNORM;
    case RSX_TEXTURE_DEPTH24_D8:     return DXGI_FORMAT_D24_UNORM_S8_UINT;
    default:                          return 0;
    }
}

#ifdef __cplusplus
}
#endif
#endif

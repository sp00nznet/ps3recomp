/*
 * ps3recomp - RSX Vertex Format Mapping
 *
 * Maps RSX vertex attribute types to host API formats.
 * RSX uses NV-style vertex attribute encoding from the NV4097 register.
 */

#ifndef PS3RECOMP_RSX_VERTEX_FORMATS_H
#define PS3RECOMP_RSX_VERTEX_FORMATS_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RSX vertex attribute type values (from NV4097_SET_VERTEX_DATA_ARRAY_FORMAT) */
#define RSX_VERTEX_TYPE_NONE           0  /* disabled */
#define RSX_VERTEX_TYPE_S1             1  /* signed normalized 16-bit */
#define RSX_VERTEX_TYPE_F              2  /* float32 */
#define RSX_VERTEX_TYPE_SF             3  /* half-float (float16) */
#define RSX_VERTEX_TYPE_UB             4  /* unsigned byte */
#define RSX_VERTEX_TYPE_S32K           5  /* signed 16-bit */
#define RSX_VERTEX_TYPE_CMP            6  /* compressed (11-11-10 packed) */
#define RSX_VERTEX_TYPE_UB256          7  /* unsigned byte (0-255, NOT normalized) */

/* Get the byte size of one component for an RSX vertex type */
static inline u32 rsx_vertex_type_size(u32 type)
{
    switch (type) {
    case RSX_VERTEX_TYPE_S1:     return 2;
    case RSX_VERTEX_TYPE_F:      return 4;
    case RSX_VERTEX_TYPE_SF:     return 2;
    case RSX_VERTEX_TYPE_UB:     return 1;
    case RSX_VERTEX_TYPE_S32K:   return 2;
    case RSX_VERTEX_TYPE_CMP:    return 4; /* packed */
    case RSX_VERTEX_TYPE_UB256:  return 1;
    default:                     return 0;
    }
}

/* Get the total byte size of a vertex attribute (type * components) */
static inline u32 rsx_vertex_attrib_size(u32 type, u32 num_components)
{
    if (type == RSX_VERTEX_TYPE_CMP) return 4; /* always 4 bytes packed */
    return rsx_vertex_type_size(type) * num_components;
}

/* DXGI format values for D3D12 input layout */
#define DXGI_FORMAT_R32_FLOAT           41
#define DXGI_FORMAT_R32G32_FLOAT        16
#define DXGI_FORMAT_R32G32B32_FLOAT     6
#define DXGI_FORMAT_R32G32B32A32_FLOAT  2
#define DXGI_FORMAT_R16_FLOAT           54
#define DXGI_FORMAT_R16G16_FLOAT        34
#define DXGI_FORMAT_R16G16B16A16_FLOAT  10
#define DXGI_FORMAT_R8_UNORM            61
#define DXGI_FORMAT_R8G8_UNORM          49
#define DXGI_FORMAT_R8G8B8A8_UNORM      28
#define DXGI_FORMAT_R8_UINT             62
#define DXGI_FORMAT_R8G8_UINT           50
#define DXGI_FORMAT_R8G8B8A8_UINT       30
#define DXGI_FORMAT_R16_SNORM           56
#define DXGI_FORMAT_R16G16_SNORM        36
#define DXGI_FORMAT_R16G16B16A16_SNORM  13

/* Map RSX vertex type + component count to DXGI format.
 * Returns 0 (DXGI_FORMAT_UNKNOWN) for unsupported combinations. */
static inline u32 rsx_to_dxgi_format(u32 type, u32 num_components)
{
    switch (type) {
    case RSX_VERTEX_TYPE_F:
        switch (num_components) {
        case 1: return DXGI_FORMAT_R32_FLOAT;
        case 2: return DXGI_FORMAT_R32G32_FLOAT;
        case 3: return DXGI_FORMAT_R32G32B32_FLOAT;
        case 4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
        break;
    case RSX_VERTEX_TYPE_SF:
        switch (num_components) {
        case 1: return DXGI_FORMAT_R16_FLOAT;
        case 2: return DXGI_FORMAT_R16G16_FLOAT;
        case 4: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        }
        break;
    case RSX_VERTEX_TYPE_UB: /* normalized unsigned byte */
        switch (num_components) {
        case 1: return DXGI_FORMAT_R8_UNORM;
        case 2: return DXGI_FORMAT_R8G8_UNORM;
        case 4: return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        break;
    case RSX_VERTEX_TYPE_UB256: /* non-normalized unsigned byte */
        switch (num_components) {
        case 1: return DXGI_FORMAT_R8_UINT;
        case 2: return DXGI_FORMAT_R8G8_UINT;
        case 4: return DXGI_FORMAT_R8G8B8A8_UINT;
        }
        break;
    case RSX_VERTEX_TYPE_S1: /* signed normalized 16-bit */
        switch (num_components) {
        case 1: return DXGI_FORMAT_R16_SNORM;
        case 2: return DXGI_FORMAT_R16G16_SNORM;
        case 4: return DXGI_FORMAT_R16G16B16A16_SNORM;
        }
        break;
    }
    return 0; /* DXGI_FORMAT_UNKNOWN */
}

#ifdef __cplusplus
}
#endif
#endif

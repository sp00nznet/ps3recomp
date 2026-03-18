/*
 * ps3recomp - RSX Command Buffer Processor
 *
 * The PS3's RSX (Reality Synthesizer) is an NV47-class GPU that receives
 * rendering commands through a ring buffer. Games write NV47xx method
 * calls to this buffer via cellGcm, and the RSX processes them.
 *
 * In recompilation, we intercept the command buffer and translate
 * NV47xx methods to the host graphics API (D3D12 on Windows, Vulkan
 * on Linux/macOS).
 *
 * Architecture:
 *   Game code → cellGcmSys (state tracking) → command buffer → RSX processor
 *                                                                  ↓
 *                                                          Host GPU backend
 *
 * Reference: NV47xx method registers documented in envytools/rnndb
 * and RPCS3's RSX module (rpcs3/Emu/RSX/).
 */

#ifndef PS3RECOMP_RSX_COMMANDS_H
#define PS3RECOMP_RSX_COMMANDS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * NV47xx Method Register Addresses
 *
 * These are the GPU method addresses that games write to the command buffer.
 * Each method has a 32-bit address and one or more 32-bit data words.
 *
 * Naming: NV4097_SET_* for 3D methods, NV3062_SET_* for 2D methods
 * -----------------------------------------------------------------------*/

/* Surface / render target configuration */
#define NV4097_SET_SURFACE_FORMAT              0x00000200
#define NV4097_SET_SURFACE_CLIP_HORIZONTAL     0x00000204
#define NV4097_SET_SURFACE_CLIP_VERTICAL       0x00000208
#define NV4097_SET_SURFACE_COLOR_AOFFSET       0x00000210
#define NV4097_SET_SURFACE_COLOR_BOFFSET       0x00000214
#define NV4097_SET_SURFACE_COLOR_COFFSET       0x00000218
#define NV4097_SET_SURFACE_COLOR_DOFFSET       0x0000021C
#define NV4097_SET_SURFACE_ZETA_OFFSET         0x00000220
#define NV4097_SET_SURFACE_COLOR_TARGET        0x00000224
#define NV4097_SET_SURFACE_PITCH_A             0x0000022C
#define NV4097_SET_SURFACE_PITCH_B             0x00000230
#define NV4097_SET_SURFACE_PITCH_C             0x00000234
#define NV4097_SET_SURFACE_PITCH_D             0x00000238
#define NV4097_SET_SURFACE_PITCH_Z             0x0000023C

/* Viewport */
#define NV4097_SET_VIEWPORT_HORIZONTAL         0x00000300
#define NV4097_SET_VIEWPORT_VERTICAL           0x00000304
#define NV4097_SET_CLIP_MIN                    0x00000394
#define NV4097_SET_CLIP_MAX                    0x00000398

/* Scissor */
#define NV4097_SET_SCISSOR_HORIZONTAL          0x000008C0
#define NV4097_SET_SCISSOR_VERTICAL            0x000008C4

/* Clear */
#define NV4097_SET_COLOR_CLEAR_VALUE           0x000001D0
#define NV4097_SET_ZSTENCIL_CLEAR_VALUE        0x000001D8
#define NV4097_CLEAR_SURFACE                   0x00001D94

/* Draw commands */
#define NV4097_SET_BEGIN_END                    0x00001808
#define NV4097_DRAW_ARRAYS                     0x00001814
#define NV4097_DRAW_INDEX_ARRAY                0x00001820

/* Vertex attributes */
#define NV4097_SET_VERTEX_DATA_ARRAY_FORMAT     0x00001740
#define NV4097_SET_VERTEX_DATA_ARRAY_OFFSET     0x00001680

/* Texture */
#define NV4097_SET_TEXTURE_OFFSET               0x00001A00
#define NV4097_SET_TEXTURE_FORMAT               0x00001A04
#define NV4097_SET_TEXTURE_ADDRESS              0x00001A08
#define NV4097_SET_TEXTURE_CONTROL0             0x00001A0C
#define NV4097_SET_TEXTURE_CONTROL1             0x00001A10
#define NV4097_SET_TEXTURE_FILTER               0x00001A14
#define NV4097_SET_TEXTURE_IMAGE_RECT           0x00001A18
#define NV4097_SET_TEXTURE_BORDER_COLOR         0x00001A1C

/* Shader programs */
#define NV4097_SET_SHADER_PROGRAM               0x000008E4
#define NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK    0x00001FF0
#define NV4097_SET_TRANSFORM_PROGRAM_LOAD       0x00001E9C
#define NV4097_SET_TRANSFORM_PROGRAM            0x00000B80
#define NV4097_SET_TRANSFORM_CONSTANT_LOAD      0x00001EFC

/* Color mask */
#define NV4097_SET_COLOR_MASK                   0x00000028

/* Alpha test */
#define NV4097_SET_ALPHA_TEST_ENABLE            0x00000104
#define NV4097_SET_ALPHA_FUNC                   0x00000108
#define NV4097_SET_ALPHA_REF                    0x0000010C

/* Blending */
#define NV4097_SET_BLEND_ENABLE                 0x00000310
#define NV4097_SET_BLEND_FUNC_SFACTOR           0x00000344
#define NV4097_SET_BLEND_FUNC_DFACTOR           0x00000348
#define NV4097_SET_BLEND_EQUATION               0x0000034C
#define NV4097_SET_BLEND_COLOR                  0x00000350

/* Depth / stencil */
#define NV4097_SET_DEPTH_TEST_ENABLE            0x00000304
#define NV4097_SET_DEPTH_FUNC                   0x00000308
#define NV4097_SET_DEPTH_MASK                   0x0000030C
#define NV4097_SET_STENCIL_TEST_ENABLE          0x00000360
#define NV4097_SET_STENCIL_FUNC                 0x00000364
#define NV4097_SET_STENCIL_FUNC_REF             0x00000368
#define NV4097_SET_STENCIL_FUNC_MASK            0x0000036C
#define NV4097_SET_STENCIL_OP_FAIL              0x00000370
#define NV4097_SET_STENCIL_OP_ZFAIL             0x00000374
#define NV4097_SET_STENCIL_OP_ZPASS             0x00000378

/* Culling */
#define NV4097_SET_CULL_FACE_ENABLE             0x000002BC
#define NV4097_SET_CULL_FACE                    0x000002C0
#define NV4097_SET_FRONT_FACE                   0x000002C4

/* Primitive types */
#define RSX_PRIMITIVE_POINTS             1
#define RSX_PRIMITIVE_LINES              2
#define RSX_PRIMITIVE_LINE_LOOP          3
#define RSX_PRIMITIVE_LINE_STRIP         4
#define RSX_PRIMITIVE_TRIANGLES          5
#define RSX_PRIMITIVE_TRIANGLE_STRIP     6
#define RSX_PRIMITIVE_TRIANGLE_FAN       7
#define RSX_PRIMITIVE_QUADS              8
#define RSX_PRIMITIVE_QUAD_STRIP         9
#define RSX_PRIMITIVE_POLYGON            10

/* ---------------------------------------------------------------------------
 * RSX State — tracked as methods are processed
 * -----------------------------------------------------------------------*/

#define RSX_MAX_TEXTURES          16
#define RSX_MAX_VERTEX_ATTRIBS    16
#define RSX_MAX_RENDER_TARGETS     4

typedef struct rsx_texture_state {
    u32 offset;
    u32 format;
    u32 address;
    u32 control0;
    u32 control1;
    u32 filter;
    u32 image_rect;
    u32 border_color;
    int dirty;
} rsx_texture_state;

typedef struct rsx_vertex_attrib {
    u32 format;
    u32 offset;
    u32 stride;
    u32 size;
    u32 type;
    int enabled;
} rsx_vertex_attrib;

typedef struct rsx_state {
    /* Render targets */
    u32 surface_format;
    u32 surface_color_offset[RSX_MAX_RENDER_TARGETS];
    u32 surface_color_pitch[RSX_MAX_RENDER_TARGETS];
    u32 surface_zeta_offset;
    u32 surface_zeta_pitch;
    u32 surface_clip_x, surface_clip_y;
    u32 surface_clip_w, surface_clip_h;
    u32 color_target;

    /* Viewport */
    u32 viewport_x, viewport_y, viewport_w, viewport_h;
    float clip_min, clip_max;

    /* Scissor */
    u32 scissor_x, scissor_y, scissor_w, scissor_h;

    /* Clear */
    u32 color_clear_value;
    u32 zstencil_clear_value;

    /* Blend */
    int blend_enable;
    u32 blend_sfactor, blend_dfactor;
    u32 blend_equation;
    u32 blend_color;

    /* Depth */
    int depth_test_enable;
    u32 depth_func;
    int depth_mask;

    /* Stencil */
    int stencil_test_enable;
    u32 stencil_func, stencil_ref, stencil_mask;
    u32 stencil_op_fail, stencil_op_zfail, stencil_op_zpass;

    /* Culling */
    int cull_face_enable;
    u32 cull_face;
    u32 front_face;

    /* Color mask — bits: [0] B, [8] G, [16] R, [24] A */
    u32 color_mask;
    int color_mask_dirty;

    /* Alpha test */
    int alpha_test_enable;
    u32 alpha_func;
    u32 alpha_ref;
    int alpha_dirty;

    /* Textures */
    rsx_texture_state textures[RSX_MAX_TEXTURES];

    /* Vertex attributes */
    rsx_vertex_attrib vertex_attribs[RSX_MAX_VERTEX_ATTRIBS];

    /* Current draw state */
    u32 primitive_type;
    int in_begin_end;  /* between BEGIN_END(type) and BEGIN_END(0) */

    /* Shader state */
    u32 shader_program;       /* fragment program address (offset | location in bits [0:1]) */
    u32 fragment_program_addr;
    u32 vertex_attrib_output_mask;
    u32 transform_program_load; /* vertex program load slot index */
    u32 transform_constant_load;
    int shader_dirty;

    /* Dirty flags for incremental state updates */
    int surface_dirty;
    int viewport_dirty;
    int blend_dirty;
    int depth_dirty;
    int stencil_dirty;
    int texture_dirty;
    int vertex_dirty;
} rsx_state;

/* ---------------------------------------------------------------------------
 * RSX Command Processor API
 * -----------------------------------------------------------------------*/

/* Initialize RSX state to defaults */
void rsx_state_init(rsx_state* state);

/* Process a single NV47xx method call.
 * method: register address (e.g., NV4097_SET_BLEND_ENABLE)
 * data:   32-bit value to write
 * Returns 0 on success, -1 if method is unrecognized. */
int rsx_process_method(rsx_state* state, u32 method, u32 data);

/* Process a command buffer segment.
 * buf:  pointer to command buffer data (host memory)
 * size: size in bytes
 * Returns number of methods processed. */
int rsx_process_command_buffer(rsx_state* state, const u32* buf, u32 size);

/* ---------------------------------------------------------------------------
 * Backend callbacks — implemented per-platform (D3D12, Vulkan, null)
 * -----------------------------------------------------------------------*/

typedef struct rsx_backend {
    void* userdata;

    /* Lifecycle */
    int  (*init)(void* userdata, u32 width, u32 height);
    void (*shutdown)(void* userdata);

    /* Frame */
    void (*begin_frame)(void* userdata);
    void (*end_frame)(void* userdata);
    void (*present)(void* userdata, u32 buffer_id);

    /* State changes (called when dirty flags are set) */
    void (*set_render_target)(void* userdata, const rsx_state* state);
    void (*set_viewport)(void* userdata, const rsx_state* state);
    void (*set_blend)(void* userdata, const rsx_state* state);
    void (*set_depth_stencil)(void* userdata, const rsx_state* state);
    void (*set_color_mask)(void* userdata, const rsx_state* state);
    void (*set_alpha_test)(void* userdata, const rsx_state* state);
    void (*set_shader)(void* userdata, const rsx_state* state);
    void (*set_vertex_attribs)(void* userdata, const rsx_state* state);

    /* Clear */
    void (*clear)(void* userdata, u32 flags, u32 color, float depth, u8 stencil);

    /* Draw */
    void (*draw_arrays)(void* userdata, u32 primitive, u32 first, u32 count);
    void (*draw_indexed)(void* userdata, u32 primitive, u32 index_offset, u32 count);

    /* Texture */
    void (*bind_texture)(void* userdata, u32 unit, const rsx_texture_state* tex);
} rsx_backend;

/* Register a backend. Call before any RSX processing. */
void rsx_set_backend(rsx_backend* backend);

/* Get the current backend (or NULL if none set). */
rsx_backend* rsx_get_backend(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_COMMANDS_H */

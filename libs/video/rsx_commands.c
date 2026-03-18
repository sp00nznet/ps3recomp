/*
 * ps3recomp - RSX Command Buffer Processor
 *
 * Parses NV47xx GPU methods from the command buffer and updates RSX state.
 * Dispatches to the registered graphics backend for actual rendering.
 *
 * Command buffer format (NV47xx FIFO):
 *   Each command is a 32-bit header followed by N data words.
 *   Header format: [31:29] type | [28:18] count | [17:13] subchannel | [12:2] method | [1:0] flags
 *
 *   Type 0 (increasing): method, method+4, method+8, ... for each data word
 *   Type 2 (non-increasing): same method repeated for each data word
 *   Type 1 (jump): jump to address in data
 *   Type 3 (call/return): call/return from subroutine
 */

#include "rsx_commands.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Global backend
 * -----------------------------------------------------------------------*/

static rsx_backend* s_backend = NULL;

void rsx_set_backend(rsx_backend* backend)
{
    s_backend = backend;
}

rsx_backend* rsx_get_backend(void)
{
    return s_backend;
}

/* ---------------------------------------------------------------------------
 * State initialization
 * -----------------------------------------------------------------------*/

void rsx_state_init(rsx_state* state)
{
    memset(state, 0, sizeof(rsx_state));

    /* Default viewport */
    state->viewport_w = 1280;
    state->viewport_h = 720;
    state->clip_min = 0.0f;
    state->clip_max = 1.0f;

    /* Default scissor */
    state->scissor_w = 4096;
    state->scissor_h = 4096;

    /* Default depth */
    state->depth_func = 1; /* LESS */
    state->depth_mask = 1;

    /* Default cull */
    state->cull_face = 1; /* BACK */
    state->front_face = 0; /* CW */

    /* Mark everything dirty */
    state->surface_dirty = 1;
    state->viewport_dirty = 1;
    state->blend_dirty = 1;
    state->depth_dirty = 1;
    state->stencil_dirty = 1;
    state->texture_dirty = 1;
    state->vertex_dirty = 1;
}

/* ---------------------------------------------------------------------------
 * Method processing
 * -----------------------------------------------------------------------*/

static int process_surface_method(rsx_state* state, u32 method, u32 data)
{
    switch (method) {
    case NV4097_SET_SURFACE_FORMAT:
        state->surface_format = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_CLIP_HORIZONTAL:
        state->surface_clip_x = data & 0xFFFF;
        state->surface_clip_w = (data >> 16) & 0xFFFF;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_CLIP_VERTICAL:
        state->surface_clip_y = data & 0xFFFF;
        state->surface_clip_h = (data >> 16) & 0xFFFF;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_AOFFSET:
        state->surface_color_offset[0] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_BOFFSET:
        state->surface_color_offset[1] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_COFFSET:
        state->surface_color_offset[2] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_DOFFSET:
        state->surface_color_offset[3] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_ZETA_OFFSET:
        state->surface_zeta_offset = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_TARGET:
        state->color_target = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_PITCH_A:
        state->surface_color_pitch[0] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_B:
        state->surface_color_pitch[1] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_C:
        state->surface_color_pitch[2] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_D:
        state->surface_color_pitch[3] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_Z:
        state->surface_zeta_pitch = data;
        return 0;
    default:
        return -1;
    }
}

int rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    /* Surface configuration */
    if (method >= 0x200 && method <= 0x23C)
        return process_surface_method(state, method, data);

    /* Viewport */
    if (method == NV4097_SET_VIEWPORT_HORIZONTAL) {
        state->viewport_x = data & 0xFFFF;
        state->viewport_w = (data >> 16) & 0xFFFF;
        state->viewport_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_VIEWPORT_VERTICAL) {
        state->viewport_y = data & 0xFFFF;
        state->viewport_h = (data >> 16) & 0xFFFF;
        state->viewport_dirty = 1;
        return 0;
    }

    /* Clear */
    if (method == NV4097_SET_COLOR_CLEAR_VALUE) {
        state->color_clear_value = data;
        return 0;
    }
    if (method == NV4097_SET_ZSTENCIL_CLEAR_VALUE) {
        state->zstencil_clear_value = data;
        return 0;
    }
    if (method == NV4097_CLEAR_SURFACE) {
        if (s_backend && s_backend->clear) {
            float depth = (float)(state->zstencil_clear_value >> 8) / (float)0xFFFFFF;
            u8 stencil = state->zstencil_clear_value & 0xFF;
            s_backend->clear(s_backend->userdata, data,
                           state->color_clear_value, depth, stencil);
        }
        return 0;
    }

    /* Blend */
    if (method == NV4097_SET_BLEND_ENABLE) {
        state->blend_enable = data ? 1 : 0;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_FUNC_SFACTOR) {
        state->blend_sfactor = data;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_FUNC_DFACTOR) {
        state->blend_dfactor = data;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_EQUATION) {
        state->blend_equation = data;
        state->blend_dirty = 1;
        return 0;
    }

    /* Depth */
    if (method == NV4097_SET_DEPTH_TEST_ENABLE) {
        state->depth_test_enable = data ? 1 : 0;
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_DEPTH_FUNC) {
        state->depth_func = data;
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_DEPTH_MASK) {
        state->depth_mask = data ? 1 : 0;
        state->depth_dirty = 1;
        return 0;
    }

    /* Culling */
    if (method == NV4097_SET_CULL_FACE_ENABLE) {
        state->cull_face_enable = data ? 1 : 0;
        return 0;
    }
    if (method == NV4097_SET_CULL_FACE) {
        state->cull_face = data;
        return 0;
    }
    if (method == NV4097_SET_FRONT_FACE) {
        state->front_face = data;
        return 0;
    }

    /* Draw */
    if (method == NV4097_SET_BEGIN_END) {
        if (data != 0) {
            state->primitive_type = data;
            state->in_begin_end = 1;

            /* Flush dirty state to backend before drawing */
            if (s_backend) {
                if (state->surface_dirty && s_backend->set_render_target)
                    s_backend->set_render_target(s_backend->userdata, state);
                if (state->viewport_dirty && s_backend->set_viewport)
                    s_backend->set_viewport(s_backend->userdata, state);
                if (state->blend_dirty && s_backend->set_blend)
                    s_backend->set_blend(s_backend->userdata, state);
                if (state->depth_dirty && s_backend->set_depth_stencil)
                    s_backend->set_depth_stencil(s_backend->userdata, state);

                state->surface_dirty = 0;
                state->viewport_dirty = 0;
                state->blend_dirty = 0;
                state->depth_dirty = 0;
                state->stencil_dirty = 0;
            }
        } else {
            state->in_begin_end = 0;
        }
        return 0;
    }

    if (method == NV4097_DRAW_ARRAYS) {
        u32 first = data & 0xFFFFFF;
        u32 count = ((data >> 24) & 0xFF) + 1;
        if (s_backend && s_backend->draw_arrays)
            s_backend->draw_arrays(s_backend->userdata, state->primitive_type, first, count);
        return 0;
    }

    /* Scissor */
    if (method == NV4097_SET_SCISSOR_HORIZONTAL) {
        state->scissor_x = data & 0xFFFF;
        state->scissor_w = (data >> 16) & 0xFFFF;
        return 0;
    }
    if (method == NV4097_SET_SCISSOR_VERTICAL) {
        state->scissor_y = data & 0xFFFF;
        state->scissor_h = (data >> 16) & 0xFFFF;
        return 0;
    }

    /* Unrecognized method — log in debug builds */
#ifndef NDEBUG
    static int s_unknown_count = 0;
    if (s_unknown_count < 50) {
        printf("[RSX] unknown method 0x%04X = 0x%08X\n", method, data);
        s_unknown_count++;
    }
#endif

    return -1;
}

/* ---------------------------------------------------------------------------
 * Command buffer parsing
 * -----------------------------------------------------------------------*/

int rsx_process_command_buffer(rsx_state* state, const u32* buf, u32 size)
{
    int methods_processed = 0;
    u32 pos = 0;
    u32 count = size / 4; /* size in dwords */

    while (pos < count) {
        u32 header = buf[pos++];
        u32 type = (header >> 29) & 0x7;

        if (type == 0 || type == 2) {
            /* Increasing or non-increasing method */
            u32 method = (header >> 2) & 0x7FF;
            method <<= 2; /* method addresses are dword-aligned */
            u32 num_data = (header >> 18) & 0x7FF;
            int increasing = (type == 0);

            for (u32 i = 0; i < num_data && pos < count; i++) {
                u32 data = buf[pos++];
                u32 m = increasing ? (method + i * 4) : method;
                rsx_process_method(state, m, data);
                methods_processed++;
            }
        } else if (type == 1) {
            /* Jump — change command buffer read position */
            /* In recomp context, this is handled by the caller */
            break;
        } else {
            /* Unknown type, skip */
            break;
        }
    }

    return methods_processed;
}

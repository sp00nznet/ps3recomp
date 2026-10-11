#include <stdlib.h>
#include <stdio.h>
/*
 * ps3recomp - NV4097 method dispatcher (Track B / LAYER 2)
 *
 * See rsx_dispatch.h for the design and the clean-room source list.
 *
 * Register/method facts used here (envytools rnndb nv30-40_3d + Mesa nv30
 * + psdevwiki; RPCS3 consulted as a read-only semantics oracle):
 *
 *   0x0194 DMA_COLOR0, 0x018C DMA_COLOR1, 0x01B4/0x01B8 DMA_COLOR2/3,
 *   0x0198 DMA_ZETA                DMA handle 0xFEED0000 = RSX local memory,
 *                                  0xFEED0001 = main (IO-mapped) memory
 *   0x0200 RT_HORIZ  x[0:15] w[16:31]      0x0204 RT_VERT y/h
 *   0x0208 RT_FORMAT color[0:4] depth[5:7] type[8:11] aa[12:15]
 *   0x020C/021C/0280/0284 COLOR0-3_PITCH   0x0210/0218/0288/028C COLOR0-3_OFFSET
 *   0x0214 ZETA_OFFSET   0x022C ZETA_PITCH   0x0220 RT_ENABLE
 *   0x0A00 VIEWPORT_HORIZ x/w   0x0A04 VIEWPORT_VERT y/h
 *   0x0A20..0x0A2C VIEWPORT_TRANSLATE[4]  0x0A30..0x0A3C VIEWPORT_SCALE[4]
 *   0x0B80..0x0BFC VP_UPLOAD_INST window (load ptr auto-advances per vec4)
 *   0x1680+i*4 VTXBUF_OFFSET[i]  bit31 = location, [0:30] = offset
 *   0x1840+i*4 TEX_SIZE1[i] (control3) pitch[0:19] depth[20:31]
 *   0x1A00+i*0x20 fragment texture unit i (16 units, 8 words each):
 *     +0x00 TEX_OFFSET   +0x04 TEX_FORMAT dma[0:1] (1 = local, 2 = main)
 *                          cube[2] border[3] dims[4:7] fmt[8:15] mips[16:19]
 *     +0x08 TEX_WRAP     +0x0C TEX_ENABLE (control0, enable = bit 31)
 *     +0x10 TEX_SWIZZLE (component remap)  +0x14 TEX_FILTER
 *     +0x18 TEX_NPOT_SIZE height[0:15] width[16:31]  +0x1C TEX_BORDER_COLOR
 *   0x1738 VERTEX_DATA_BASE_OFFSET  0x173C VB_ELEMENT_BASE
 *   0x1740+i*4 VTXFMT[i] type[0:3] size[4:7] stride[8:15] frequency[16:31]
 *   0x1808 VERTEX_BEGIN_END  arg = primitive, 0 = end
 *   0x1814 VB_VERTEX_BATCH   first[0:23] (count-1)[24:31]
 *   0x1818 INLINE_ARRAY     vertex data pushed through the FIFO itself
 *   0x181C IDXBUF_OFFSET     0x1820 IDXBUF_FORMAT location[0:3] type[4:11]
 *   0x1824 VB_INDEX_BATCH    first[0:23] (count-1)[24:31]
 *   0x1D8C CLEAR_DEPTH_VALUE  0x1D90 CLEAR_COLOR_VALUE (A8R8G8B8)
 *   0x1D94 CLEAR_BUFFERS     Z=0x01 S=0x02 R=0x10 G=0x20 B=0x40 A=0x80
 *   0x1E9C VP_UPLOAD_FROM_ID (transform program load pointer)
 *   0x1EFC VP_UPLOAD_CONST_ID (transform constant load pointer; NOT
 *          auto-advancing — every burst re-reads it)
 *   0x1F00..0x1F7C VP_UPLOAD_CONST window: word i -> slot load + i/4, lane i%4
 *   0xE944 gcm driver-queue flip (established empirically from this title)
 */

#include "rsx_dispatch.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define M_DMA_COLOR1            0x018C
#define M_DMA_COLOR0            0x0194
#define M_DMA_ZETA              0x0198
#define M_DMA_COLOR2            0x01B4
#define M_DMA_COLOR3            0x01B8
#define M_RT_HORIZ              0x0200
#define M_RT_VERT               0x0204
#define M_RT_FORMAT             0x0208
#define M_COLOR0_PITCH          0x020C
#define M_COLOR0_OFFSET         0x0210
#define M_ZETA_OFFSET           0x0214
#define M_COLOR1_OFFSET         0x0218
#define M_COLOR1_PITCH          0x021C
#define M_RT_ENABLE             0x0220
#define M_ZETA_PITCH            0x022C
#define M_COLOR2_PITCH          0x0280
#define M_COLOR3_PITCH          0x0284
#define M_COLOR2_OFFSET         0x0288
#define M_COLOR3_OFFSET         0x028C
#define M_VIEWPORT_HORIZ        0x0A00
#define M_VIEWPORT_VERT         0x0A04
#define M_VIEWPORT_TRANSLATE    0x0A20
#define M_VIEWPORT_SCALE        0x0A30
#define M_VP_UPLOAD_INST        0x0B80
#define M_VTXBUF_OFFSET         0x1680
#define M_VERTEX_DATA_BASE      0x1738
#define M_VB_ELEMENT_BASE       0x173C
#define M_VTXFMT                0x1740
#define M_VERTEX_BEGIN_END      0x1808
#define M_VB_VERTEX_BATCH       0x1814
#define M_INLINE_ARRAY          0x1818
#define M_IDXBUF_OFFSET         0x181C
#define M_IDXBUF_FORMAT         0x1820
#define M_VB_INDEX_BATCH        0x1824
#define M_TEX_SIZE1             0x1840  /* + unit * 4  (control3)           */
#define M_TEX_OFFSET            0x1A00  /* + unit * 0x20 (8-word unit block) */
#define M_VERTEX_TEXTURE        0x0900  /* + unit * 0x20 (8-word unit block) */
#define M_FP_ACTIVE_PROGRAM     0x08E4  /* offset | dma-context [1:0]        */
#define M_FP_CONTROL            0x1D60
#define M_VP_START_FROM_ID      0x1EA0
#define M_VTX_ATTR_4F           0x1C00  /* + attr * 0x10, 4 float words      */
#define M_CLEAR_DEPTH_VALUE     0x1D8C
#define M_CLEAR_COLOR_VALUE     0x1D90
#define M_CLEAR_BUFFERS         0x1D94
#define M_VP_UPLOAD_FROM_ID     0x1E9C
#define M_VP_UPLOAD_CONST_ID    0x1EFC
#define M_VP_UPLOAD_CONST       0x1F00
#define M_GCM_FLIP_HEAD_0       0xE920
#define M_GCM_FLIP_HEAD_1       0xE924
#define M_GCM_DRIVER_FLIP       0xE944
/* Primitive restart: a sentinel index value that breaks a TRIANGLE_STRIP/
 * FAN mid-draw with no connecting triangle (RPCS3 oracle: gcm_enums.h
 * NV4097_SET_RESTART_INDEX_ENABLE/NV4097_SET_RESTART_INDEX; semantics in
 * rsx_methods.h restart_index_enabled()/restart_index() -- enabled only
 * when the enable bit is set AND, for 16-bit index buffers, the configured
 * value fits in 16 bits). */
#define M_RESTART_INDEX_ENABLE  0x1DAC
#define M_RESTART_INDEX         0x1DB0

/* Render state consumed directly by the replay harness's PSO builder
 * (rsx_dsp_reg reads straight out of the register file, so these already
 * took effect before being classified here; this fixes the coverage report
 * to match reality instead of showing them as TODO). Addresses from
 * tools/nv40_methods.py (envytools rnndb). */
#define M_ALPHA_FUNC_ENABLE     0x0304
#define M_ALPHA_FUNC_FUNC       0x0308
#define M_ALPHA_FUNC_REF        0x030C
#define M_BLEND_FUNC_ENABLE     0x0310
#define M_BLEND_FUNC_SRC        0x0314
#define M_BLEND_FUNC_DST        0x0318
#define M_COLOR_MASK            0x0324
#define M_MRT_COLOR_MASK        0x0370
#define M_DEPTH_FUNC            0x0A6C
#define M_DEPTH_WRITE_ENABLE    0x0A70
#define M_DEPTH_TEST_ENABLE     0x0A74
#define M_CULL_FACE             0x1830
#define M_FRONT_FACE            0x1834
#define M_CULL_FACE_ENABLE      0x183C
#define M_VTX_CACHE_INVALIDATE  0x1714
#define M_NV4097_NOP            0x0100
#define M_VP_ATTRIB_EN          0x1FF0
#define M_VP_RESULT_EN          0x1FF4

#define DMA_HANDLE_LOCAL        0xFEED0000u
#define DMA_HANDLE_MAIN         0xFEED0001u

static u32 dma_to_location(u32 handle)
{
    return handle == DMA_HANDLE_MAIN ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
}

static void mark_class(rsx_dispatch* rsx, u32 method, u32 count, u8 klass)
{
    for (u32 i = 0; i < count; i++)
        rsx->klass[(method >> 2) + i] = klass;
}

void rsx_dispatch_init(rsx_dispatch* rsx, const rsx_dispatch_sink* sink)
{
    memset(rsx, 0, sizeof(*rsx));
    if (sink)
        rsx->sink = *sink;

    /* s31 (scratch/s31_blue_emitter.md): the nv40 RESET DEFAULT for
     * COLOR_MASK is all-channels-on (RPCS3 rsx_methods.cpp:345,983 seeds
     * registers[NV4097_SET_COLOR_MASK]=0x1010101). Seeding it here makes a
     * raw register read trustworthy for every consumer: a game-written 0 is
     * a REAL "write no color channels" (the character shadow-mask
     * depth-prime pass), distinguishable from never-initialized. Without
     * the seed, consumers substituted mask=ALL for 0 and force-wrote the
     * depth-only pass's garbage FP output (the blue-character class). */
    rsx->regs[M_COLOR_MASK >> 2] = 0x01010101u;

    /* NV40 reset state is counter-clockwise front winding.  Leaving this
     * register zero made a live stream that relied on the hardware default
     * cull the exterior of orphanage meshes, while captured replay was correct
     * because its seeded register image already contained 0x901. */
    rsx->regs[M_FRONT_FACE >> 2] = 0x0901u;

    /* NV40 reset ZSTENCIL_CLEAR_VALUE (Z24S8: depth24<<8 | stencil8) is
     * 0xFFFFFF00 = depth 1.0, stencil 0 (RPCS3 rsx_methods.cpp reset image
     * as numbering oracle). Seeded so a raw register read is trustworthy for
     * clear consumers, mirroring COLOR_MASK/FRONT_FACE above. */
    rsx->regs[0x1D8C >> 2] = 0xFFFFFF00u;

    /* Execution methods */
    mark_class(rsx, M_VERTEX_BEGIN_END,  1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_VB_VERTEX_BATCH,   1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_INLINE_ARRAY,      1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_VB_INDEX_BATCH,    1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_CLEAR_BUFFERS,     1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_VP_UPLOAD_INST,   32, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_VP_UPLOAD_CONST,  32, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_GCM_FLIP_HEAD_0,   1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_GCM_FLIP_HEAD_1,   1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_GCM_DRIVER_FLIP,   1, RSX_DSP_CLASS_EXEC);

    /* State the getters decode */
    mark_class(rsx, M_DMA_COLOR1,        1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_DMA_COLOR0,        2, RSX_DSP_CLASS_STATE); /* +ZETA */
    mark_class(rsx, M_DMA_COLOR2,        2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_RT_HORIZ,         12, RSX_DSP_CLASS_STATE); /* 0x200..0x22C */
    mark_class(rsx, M_COLOR2_PITCH,      4, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VIEWPORT_HORIZ,    2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VIEWPORT_TRANSLATE, 8, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VTXBUF_OFFSET,    16, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VERTEX_DATA_BASE,  2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VTXFMT,           16, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_IDXBUF_OFFSET,     2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_TEX_SIZE1,        16, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_TEX_OFFSET, RSX_DSP_NUM_TEXTURES * 8, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_CLEAR_DEPTH_VALUE, 2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_UPLOAD_FROM_ID, 1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_RESTART_INDEX_ENABLE, 2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_UPLOAD_CONST_ID, 1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_FP_ACTIVE_PROGRAM, 1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_FP_CONTROL,        1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_START_FROM_ID,  1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VTX_ATTR_4F,      64, RSX_DSP_CLASS_STATE);

    /* Alpha test / blend / color-mask / depth / cull: rsx_dsp_reg reads these
     * straight from the register file (replay_main.c's decode_render_state,
     * color-mask application), so they were already live; this only corrects
     * the coverage report. */
    mark_class(rsx, M_ALPHA_FUNC_ENABLE, 3, RSX_DSP_CLASS_STATE); /* ..FUNC_REF */
    mark_class(rsx, M_BLEND_FUNC_ENABLE, 5, RSX_DSP_CLASS_STATE); /* ..BLEND_EQUATION */
    mark_class(rsx, M_COLOR_MASK,        1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_MRT_COLOR_MASK,    1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_DEPTH_FUNC,        3, RSX_DSP_CLASS_STATE); /* ..TEST_ENABLE */
    mark_class(rsx, M_CULL_FACE,         1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_FRONT_FACE,        1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_CULL_FACE_ENABLE,  1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_ATTRIB_EN,      1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_RESULT_EN,      1, RSX_DSP_CLASS_STATE);

    /* Explicit no-ops: the vertex cache invalidate hint and the reserved
     * NOP method carry no state the replay harness needs (there is no CPU
     * vertex cache to invalidate; every draw re-fetches from guest memory
     * each time). Classifying them EXEC removes them from the TODO count
     * without pretending they do anything. */
    mark_class(rsx, M_VTX_CACHE_INVALIDATE, 1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_NV4097_NOP,           1, RSX_DSP_CLASS_EXEC);
}

void rsx_dispatch_seed_registers(rsx_dispatch* rsx, const u32* regs, u32 count)
{
    if (count > RSX_DSP_NUM_REGS)
        count = RSX_DSP_NUM_REGS;
    memcpy(rsx->regs, regs, count * sizeof(u32));
}

void rsx_dispatch_seed_transform_program(rsx_dispatch* rsx, const u32* words, u32 count)
{
    if (count > RSX_DSP_VP_WORDS)
        count = RSX_DSP_VP_WORDS;
    memcpy(rsx->vp, words, count * sizeof(u32));
}

void rsx_dispatch_seed_transform_constants(rsx_dispatch* rsx, const u32* words, u32 count)
{
    const u32 max_words = RSX_DSP_NUM_CONSTANTS * 4;
    if (count > max_words)
        count = max_words;
    memcpy(rsx->constants, words, count * sizeof(u32));
}


/* ---- immediate mode ----------------------------------------------------
 * SET_VERTEX_DATA*_M: per-attribute register writes. Outside BEGIN/END they
 * set the value a disabled attribute reads; inside, they also build vertices
 * (RPCS3 rsx::util::push_vertex_data). The value is stored converted to a
 * float4 in the VTX_ATTR_4F block too, so rsx_dsp_vertex_default sees writes
 * of every width. Packing follows libgcm's cellGcmSetVertexData* helpers:
 * 4UB is x | y<<8 | z<<16 | w<<24, 2S/4S are x | y<<16 per dword. */
static float imm_bits_f(u32 v) { float f; memcpy(&f, &v, 4); return f; }

static void imm_store(rsx_dispatch* rsx, u32 attr, u32 dword, u32 dwords_per_vertex,
                      const float* v, u32 first, u32 n, u32 size)
{
    float* cur = rsx->imm_val[attr];
    if (dword == 0) {   /* a new value starts: missing components default (0,0,0,1) */
        for (u32 i = size; i < 4; i++) cur[i] = (i == 3) ? 1.0f : 0.0f;
    }
    for (u32 i = 0; i < n; i++) cur[first + i] = v[i];
    for (u32 i = 0; i < 4; i++) {
        u32 b; memcpy(&b, &cur[i], 4);
        rsx->regs[(M_VTX_ATTR_4F + attr * 0x10 + i * 4) >> 2] = b;
    }
    if (!rsx->in_begin_end) return;
    rsx->imm_mask |= 1u << attr;
    if (attr != 0) return;
    if (++rsx->imm_a0_dw < dwords_per_vertex) return;
    rsx->imm_a0_dw = 0;
    if (rsx->imm_n >= RSX_DSP_IMM_MAX) { rsx->imm_dropped++; return; }
    memcpy(rsx->imm_vert[rsx->imm_n++], rsx->imm_val, sizeof rsx->imm_val);
}

/* Returns 1 when method is an immediate vertex-data write (handled). */
static int imm_method(rsx_dispatch* rsx, u32 m, u32 arg)
{
    float v[4];
    if (m >= 0x1C00 && m < 0x1D00) {            /* DATA4F_M   */
        const u32 a = (m - 0x1C00) >> 4, c = ((m - 0x1C00) >> 2) & 3;
        v[0] = imm_bits_f(arg);
        imm_store(rsx, a, c, 4, v, c, 1, 4);
        return 1;
    }
    if (m >= 0x1880 && m < 0x1900) {            /* DATA2F_M   */
        const u32 a = (m - 0x1880) >> 3, c = ((m - 0x1880) >> 2) & 1;
        v[0] = imm_bits_f(arg);
        imm_store(rsx, a, c, 2, v, c, 1, 2);
        return 1;
    }
    if (m >= 0x1500 && m < 0x15C0) {            /* DATA3F_M   */
        const u32 a = (m - 0x1500) / 12, c = ((m - 0x1500) >> 2) % 3;
        v[0] = imm_bits_f(arg);
        imm_store(rsx, a, c, 3, v, c, 1, 3);
        return 1;
    }
    if (m >= 0x1E40 && m < 0x1E80) {            /* DATA1F_M   */
        v[0] = imm_bits_f(arg);
        imm_store(rsx, (m - 0x1E40) >> 2, 0, 1, v, 0, 1, 1);
        return 1;
    }
    if (m >= 0x1940 && m < 0x1980) {            /* DATA4UB_M (normalized) */
        for (u32 i = 0; i < 4; i++) v[i] = (float)((arg >> (8 * i)) & 0xFF) / 255.0f;
        imm_store(rsx, (m - 0x1940) >> 2, 0, 1, v, 0, 4, 4);
        return 1;
    }
    if (m >= 0x1900 && m < 0x1940) {            /* DATA2S_M   */
        v[0] = (float)(int16_t)(arg & 0xFFFF);
        v[1] = (float)(int16_t)(arg >> 16);
        imm_store(rsx, (m - 0x1900) >> 2, 0, 1, v, 0, 2, 2);
        return 1;
    }
    if (m >= 0x1980 && m < 0x1A00) {            /* DATA4S_M   */
        const u32 a = (m - 0x1980) >> 3, d = ((m - 0x1980) >> 2) & 1;
        v[0] = (float)(int16_t)(arg & 0xFFFF);
        v[1] = (float)(int16_t)(arg >> 16);
        imm_store(rsx, a, d, 2, v, 2 * d, 2, 4);
        return 1;
    }
    if (m >= 0x0A80 && m < 0x0B00) {            /* DATA_SCALED4S_M (normalized) */
        const u32 a = (m - 0x0A80) >> 3, d = ((m - 0x0A80) >> 2) & 1;
        v[0] = (float)(int16_t)(arg & 0xFFFF) / 32767.0f;
        v[1] = (float)(int16_t)(arg >> 16) / 32767.0f;
        imm_store(rsx, a, d, 2, v, 2 * d, 2, 4);
        return 1;
    }
    return 0;
}

/* BEGIN_END(0) of a pair that carried immediate vertices and no array,
 * index or inline packet: replay them as an INLINE_ARRAY of float4s, with
 * VTXFMT rewritten for the duration so every consumer's inline layout reads
 * exactly the written attributes and the rest fall back to their register
 * value. */
static void imm_flush_as_inline(rsx_dispatch* rsx)
{
    u32 k = 0;
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) if (rsx->imm_mask & (1u << a)) k++;
    const u32 stride = 16u * k;
    u32 n = rsx->imm_n;
    if (n * stride > RSX_DSP_INLINE_MAX_BYTES) n = RSX_DSP_INLINE_MAX_BYTES / stride;
    u32 saved[RSX_DSP_NUM_VERTEX_ATTR];
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        saved[a] = rsx->regs[(M_VTXFMT >> 2) + a];
        rsx->regs[(M_VTXFMT >> 2) + a] =
            (rsx->imm_mask & (1u << a)) ? (2u | (4u << 4) | (stride << 8)) : 0u;
    }
    u8* o = rsx->inline_data;
    for (u32 i = 0; i < n; i++)
        for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
            if (!(rsx->imm_mask & (1u << a))) continue;
            for (u32 c = 0; c < 4; c++) {
                u32 b; memcpy(&b, &rsx->imm_vert[i][a][c], 4);
                *o++ = (u8)(b >> 24); *o++ = (u8)(b >> 16); *o++ = (u8)(b >> 8); *o++ = (u8)b;
            }
        }
    if (rsx->sink.inline_array)
        rsx->sink.inline_array(rsx->sink.user, rsx, rsx->inline_data, n * stride);
    if (rsx->sink.end)
        rsx->sink.end(rsx->sink.user, rsx);
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++)
        rsx->regs[(M_VTXFMT >> 2) + a] = saved[a];
}

/* RSX_EMPTY_DRAW_LOG=<flip>: from that flip on, name the methods written
 * inside a BEGIN_END pair that carried no vertex packet (first 12 pairs). */
static u32 s_ed_flips, s_ed_n, s_ed_packets, s_ed_printed;
static u32 s_ed_m[24], s_ed_a[24];
static int s_ed_on = -1; static u32 s_ed_from;

void rsx_dispatch_method(rsx_dispatch* rsx, u32 method, u32 arg)
{
    method &= 0xFFFFC;
    const u32 idx = method >> 2;
    /* regs[] covers methods 0..0xFFFC (subchannel bits included). A larger
     * value is not a method -- and indexing with it wrote past regs[] into
     * vp[] and constants[]: Tornado Outbreak's c[461] held two copies of a
     * "load constant 461" command header, i.e. FIFO words stored as shader
     * constants. Drop it, and say what it was. */
    if (idx >= RSX_DSP_NUM_REGS) {
        static int n = 0;
        if (n++ < 8)
            fprintf(stderr, "[rsx-dispatch] method 0x%05X (arg 0x%08X) out of range -- dropped\n",
                    method, arg);
        return;
    }
    if (s_ed_on < 0) { const char* e = getenv("RSX_EMPTY_DRAW_LOG"); s_ed_on = e ? 1 : 0; s_ed_from = e ? (u32)atoi(e) : 0; }
    if (s_ed_on) {
        if (method == M_GCM_FLIP_HEAD_0 || method == M_GCM_FLIP_HEAD_1 || method == M_GCM_DRIVER_FLIP) s_ed_flips++;
        if (method == M_VERTEX_BEGIN_END && arg) { s_ed_n = 0; s_ed_packets = 0; }
        else if (method == M_VERTEX_BEGIN_END) {
            if (!s_ed_packets && s_ed_flips >= s_ed_from && s_ed_printed < 12) {
                s_ed_printed++;
                fprintf(stderr, "[rsx empty-draw] flip=%u prim=%u methods(%u):", s_ed_flips, rsx->current_primitive, s_ed_n);
                for (u32 i = 0; i < s_ed_n && i < 24; i++) fprintf(stderr, " %04X=%08X", s_ed_m[i], s_ed_a[i]);
                fputc('\n', stderr);
            }
        } else if (rsx->in_begin_end) {
            if (method == M_INLINE_ARRAY || method == M_VB_VERTEX_BATCH || method == M_VB_INDEX_BATCH) s_ed_packets++;
            if (s_ed_n < 24) { s_ed_m[s_ed_n] = method; s_ed_a[s_ed_n] = arg; } s_ed_n++;
        }
    }
    rsx->seen[idx]++;
    rsx->regs[idx] = arg;

    /* Transform program upload window: word goes to instruction slot
     * VP_UPLOAD_FROM_ID; the load pointer advances after every completed
     * vec4 (hardware auto-increment, confirmed against the RPCS3 oracle). */
    if (method >= M_VP_UPLOAD_INST && method < M_VP_UPLOAD_INST + 32 * 4) {
        const u32 lane = ((method - M_VP_UPLOAD_INST) >> 2) & 3;
        u32* load = &rsx->regs[M_VP_UPLOAD_FROM_ID >> 2];
        const u32 pos = *load * 4 + lane;
        if (pos < RSX_DSP_VP_WORDS)
            rsx->vp[pos] = arg;
        if (lane == 3)
            (*load)++;
        return;
    }

    /* Transform constant upload window: word i of the burst writes
     * constants[VP_UPLOAD_CONST_ID + i/4][i%4]; the load pointer does NOT
     * advance. */
    if (method >= M_VP_UPLOAD_CONST && method < M_VP_UPLOAD_CONST + 32 * 4) {
        const u32 word = (method - M_VP_UPLOAD_CONST) >> 2;
        const u32 slot = rsx->regs[M_VP_UPLOAD_CONST_ID >> 2] + (word >> 2);
        if (slot < RSX_DSP_NUM_CONSTANTS)
            rsx->constants[slot][word & 3] = arg;
        /* RSX_CONST_LOG=<lo>,<hi>: every transform-constant upload into that
         * slot range (first 400) -- what a shader's constants really were. */
        { static int lo = -2, hi = 0, n = 0;
          if (lo == -2) { const char* e = getenv("RSX_CONST_LOG"); lo = -1;
                          if (e) sscanf(e, "%d,%d", &lo, &hi); }
          if (lo >= 0 && (int)slot >= lo && (int)slot <= hi && n < 400) { n++;
              fprintf(stderr, "[const-load] c[%u].%c = 0x%08X (method 0x%04X id=%u)\n", slot,
                      "xyzw"[word & 3], arg, method, rsx->regs[M_VP_UPLOAD_CONST_ID >> 2]); } }
        return;
    }

    if (imm_method(rsx, method, arg))
        return;

    switch (method) {
    case M_CLEAR_BUFFERS:
        if (rsx->sink.clear)
            rsx->sink.clear(rsx->sink.user, rsx, arg);
        break;

    case M_VERTEX_BEGIN_END:
        if (arg) {
            rsx->in_begin_end = 1;
            rsx->current_primitive = arg;
            rsx->inline_len = 0;
            rsx->imm_mask = 0; rsx->imm_a0_dw = 0; rsx->imm_n = 0;
            rsx->pair_packets = 0;
            if (rsx->sink.begin)
                rsx->sink.begin(rsx->sink.user, rsx, arg);
        } else {
            rsx->in_begin_end = 0;
            static int imm_on = -1;   /* RSX_IMM=0 disables immediate-mode draws */
            if (imm_on < 0) { const char* e = getenv("RSX_IMM"); imm_on = !(e && *e == '0'); }
            /* RSX_IMM_SKIP_RT=<hex offset>: drop immediate pairs drawing into
             * colour target A at that offset (bisecting a post chain). */
            static long imm_skip = -2;
            if (imm_skip == -2) { const char* e = getenv("RSX_IMM_SKIP_RT"); imm_skip = e ? (long)strtoul(e, 0, 16) : -1; }
            if (imm_on && imm_skip >= 0 && rsx->regs[0x0210 >> 2] == (u32)imm_skip) {
                rsx->imm_n = 0;
                if (rsx->sink.end) rsx->sink.end(rsx->sink.user, rsx);
                break;
            }
            if (imm_on && !rsx->pair_packets && rsx->imm_n && rsx->imm_mask) {
                imm_flush_as_inline(rsx);
                rsx->imm_n = 0;
                break;
            }
            /* Hand an inline stream over BEFORE end: the sink reads end as
             * "draw what you have", so a batch delivered after it would be
             * drawn one primitive late, or not at all. */
            if (rsx->inline_len && rsx->sink.inline_array)
                rsx->sink.inline_array(rsx->sink.user, rsx,
                                       rsx->inline_data, rsx->inline_len);
            rsx->inline_len = 0;
            if (rsx->sink.end)
                rsx->sink.end(rsx->sink.user, rsx);
        }
        break;

    /* INLINE_ARRAY: the vertex stream travels IN the pushbuffer rather than
     * being fetched from a vertex array, so there is no (first, count) --
     * every word between BEGIN_END(prim) and BEGIN_END(0) is vertex data.
     * The layout is still the ordinary VTXFMT declaration: enabled
     * attributes packed in ascending index order, one vertex every stride
     * bytes. Store the words back in guest byte order so the consumer
     * decodes them with the same big-endian readers a vertex array uses;
     * the dispatcher never interprets them.
     *
     * A title that builds geometry this way (Guitar Hero III draws its
     * entire 2D layer as inline QUADS) previously issued draws that arrived
     * carrying no vertices at all: the backend saw BEGIN/END with nothing
     * between them and counted an empty group. */
    case M_INLINE_ARRAY:
        rsx->pair_packets++;
        if (rsx->inline_len + 4u <= RSX_DSP_INLINE_MAX_BYTES) {
            rsx->inline_data[rsx->inline_len + 0] = (u8)(arg >> 24);
            rsx->inline_data[rsx->inline_len + 1] = (u8)(arg >> 16);
            rsx->inline_data[rsx->inline_len + 2] = (u8)(arg >> 8);
            rsx->inline_data[rsx->inline_len + 3] = (u8)arg;
            rsx->inline_len += 4;
        } else if (rsx->inline_len) {
            /* Truncating leaves a torn final vertex; drop the whole stream
             * and count it instead of drawing something half-read. */
            rsx->inline_len = 0;
            rsx->inline_dropped++;
        }
        break;

    case M_VB_VERTEX_BATCH:
        rsx->pair_packets++;
        if (rsx->sink.draw_arrays)
            rsx->sink.draw_arrays(rsx->sink.user, rsx,
                                  arg & 0xFFFFFF, (arg >> 24) + 1);
        break;

    case M_VB_INDEX_BATCH:
        rsx->pair_packets++;
        if (rsx->sink.draw_index_array)
            rsx->sink.draw_index_array(rsx->sink.user, rsx,
                                       arg & 0xFFFFFF, (arg >> 24) + 1);
        break;

    case M_GCM_FLIP_HEAD_0:
    case M_GCM_FLIP_HEAD_1:
    case M_GCM_DRIVER_FLIP:
        if (rsx->sink.flip)
            rsx->sink.flip(rsx->sink.user, rsx, arg);
        break;

    default:
        break;
    }
}

/* ---- getters ----------------------------------------------------------- */

u32 rsx_dsp_clear_color(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_CLEAR_COLOR_VALUE);
}

u32 rsx_dsp_clear_zstencil(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_CLEAR_DEPTH_VALUE);
}

void rsx_dsp_get_surface(const rsx_dispatch* rsx, rsx_dsp_surface* out)
{
    const u32 fmt = rsx_dsp_reg(rsx, M_RT_FORMAT);
    out->color_format = fmt & 0x1F;
    out->depth_format = (fmt >> 5) & 7;
    out->raster_type  = (fmt >> 8) & 0xF;

    const u32 h = rsx_dsp_reg(rsx, M_RT_HORIZ);
    const u32 v = rsx_dsp_reg(rsx, M_RT_VERT);
    out->clip_x = h & 0xFFFF;
    out->clip_w = h >> 16;
    out->clip_y = v & 0xFFFF;
    out->clip_h = v >> 16;

    out->color_offset[0] = rsx_dsp_reg(rsx, M_COLOR0_OFFSET);
    out->color_offset[1] = rsx_dsp_reg(rsx, M_COLOR1_OFFSET);
    out->color_offset[2] = rsx_dsp_reg(rsx, M_COLOR2_OFFSET);
    out->color_offset[3] = rsx_dsp_reg(rsx, M_COLOR3_OFFSET);
    out->color_pitch[0]  = rsx_dsp_reg(rsx, M_COLOR0_PITCH);
    out->color_pitch[1]  = rsx_dsp_reg(rsx, M_COLOR1_PITCH);
    out->color_pitch[2]  = rsx_dsp_reg(rsx, M_COLOR2_PITCH);
    out->color_pitch[3]  = rsx_dsp_reg(rsx, M_COLOR3_PITCH);
    out->color_location[0] = dma_to_location(rsx_dsp_reg(rsx, M_DMA_COLOR0));
    out->color_location[1] = dma_to_location(rsx_dsp_reg(rsx, M_DMA_COLOR1));
    out->color_location[2] = dma_to_location(rsx_dsp_reg(rsx, M_DMA_COLOR2));
    out->color_location[3] = dma_to_location(rsx_dsp_reg(rsx, M_DMA_COLOR3));
    out->color_target  = rsx_dsp_reg(rsx, M_RT_ENABLE);
    out->zeta_offset   = rsx_dsp_reg(rsx, M_ZETA_OFFSET);
    out->zeta_pitch    = rsx_dsp_reg(rsx, M_ZETA_PITCH);
    out->zeta_location = dma_to_location(rsx_dsp_reg(rsx, M_DMA_ZETA));
}

void rsx_dsp_get_viewport(const rsx_dispatch* rsx, rsx_dsp_viewport* out)
{
    const u32 h = rsx_dsp_reg(rsx, M_VIEWPORT_HORIZ);
    const u32 v = rsx_dsp_reg(rsx, M_VIEWPORT_VERT);
    out->x = h & 0xFFFF;
    out->w = h >> 16;
    out->y = v & 0xFFFF;
    out->h = v >> 16;
    for (u32 i = 0; i < 4; i++) {
        const u32 t = rsx_dsp_reg(rsx, M_VIEWPORT_TRANSLATE + i * 4);
        const u32 s = rsx_dsp_reg(rsx, M_VIEWPORT_SCALE + i * 4);
        memcpy(&out->translate[i], &t, 4);
        memcpy(&out->scale[i], &s, 4);
    }
}

void rsx_dsp_get_vertex_attr(const rsx_dispatch* rsx, u32 index, rsx_dsp_vertex_attr* out)
{
    memset(out, 0, sizeof(*out));
    if (index >= RSX_DSP_NUM_VERTEX_ATTR)
        return;
    const u32 fmt = rsx_dsp_reg(rsx, M_VTXFMT + index * 4);
    const u32 off = rsx_dsp_reg(rsx, M_VTXBUF_OFFSET + index * 4);
    out->type      = fmt & 7;
    out->size      = (fmt >> 4) & 0xF;
    out->stride    = (fmt >> 8) & 0xFF;
    out->frequency = fmt >> 16;
    out->offset    = off & 0x7FFFFFFF;
    out->location  = off >> 31;
}

void rsx_dsp_get_texture(const rsx_dispatch* rsx, u32 unit, rsx_dsp_texture* out)
{
    memset(out, 0, sizeof(*out));
    if (unit >= RSX_DSP_NUM_TEXTURES)
        return;
    const u32 base = M_TEX_OFFSET + unit * 0x20;
    const u32 fmt  = rsx_dsp_reg(rsx, base + 0x04);
    out->offset    = rsx_dsp_reg(rsx, base);
    out->location  = (fmt & 3) == 2 ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
    out->cubemap   = (fmt >> 2) & 1;
    out->dimension = (fmt >> 4) & 0xF;
    out->format    = (fmt >> 8) & 0xFF;
    out->mipmaps   = fmt >> 16;
    out->wrap      = rsx_dsp_reg(rsx, base + 0x08);
    out->control0  = rsx_dsp_reg(rsx, base + 0x0C);
    out->enabled   = out->control0 >> 31;
    out->remap     = rsx_dsp_reg(rsx, base + 0x10);
    out->filter    = rsx_dsp_reg(rsx, base + 0x14);
    const u32 rect = rsx_dsp_reg(rsx, base + 0x18);
    out->width     = rect >> 16;
    out->height    = rect & 0xFFFF;
    out->border_color = rsx_dsp_reg(rsx, base + 0x1C);
    const u32 sz1  = rsx_dsp_reg(rsx, M_TEX_SIZE1 + unit * 4);
    out->pitch     = sz1 & 0xFFFFF;
    out->depth     = sz1 >> 20;
}

void rsx_dsp_get_vertex_texture(const rsx_dispatch* rsx, u32 unit,
                                rsx_dsp_vertex_texture* out)
{
    memset(out, 0, sizeof(*out));
    if (unit >= RSX_DSP_NUM_VERTEX_TEXTURES)
        return;
    const u32 base = M_VERTEX_TEXTURE + unit * 0x20;
    const u32 fmt  = rsx_dsp_reg(rsx, base + 0x04);
    out->offset    = rsx_dsp_reg(rsx, base + 0x00);
    out->location  = (fmt & 3) == 2 ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
    out->cubemap   = (fmt >> 2) & 1;
    out->dimension = (fmt >> 4) & 0xF;
    out->format    = (fmt >> 8) & 0xFF;
    out->mipmaps   = fmt >> 16;
    out->wrap      = rsx_dsp_reg(rsx, base + 0x08);
    out->control0  = rsx_dsp_reg(rsx, base + 0x0C);
    out->enabled   = out->control0 >> 31;
    const u32 ctl3 = rsx_dsp_reg(rsx, base + 0x10);
    out->pitch     = ctl3 & 0xFFFFF;
    out->depth     = ctl3 >> 20;
    out->filter    = rsx_dsp_reg(rsx, base + 0x14);
    const u32 rect = rsx_dsp_reg(rsx, base + 0x18);
    out->width     = rect >> 16;
    out->height    = rect & 0xFFFF;
    out->border_color = rsx_dsp_reg(rsx, base + 0x1C);
}

u32 rsx_dsp_vertex_data_base_offset(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_VERTEX_DATA_BASE);
}

u32 rsx_dsp_vertex_data_base_index(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_VB_ELEMENT_BASE);
}

u32 rsx_dsp_fragment_program(const rsx_dispatch* rsx, u32* location)
{
    const u32 v = rsx_dsp_reg(rsx, M_FP_ACTIVE_PROGRAM);
    if (location)
        *location = (v & 3) == 2 ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
    return v & ~3u;
}

u32 rsx_dsp_shader_control(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_FP_CONTROL);
}

u32 rsx_dsp_vp_start(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_VP_START_FROM_ID);
}

int rsx_dsp_restart_index_enabled(const rsx_dispatch* rsx, int index_is_u32)
{
    if (!rsx_dsp_reg(rsx, M_RESTART_INDEX_ENABLE))
        return 0;
    /* RPCS3 oracle (rsx_methods.h restart_index_enabled()): a 16-bit index
     * buffer disables restart entirely if the configured sentinel doesn't
     * fit in 16 bits, rather than truncating it. */
    if (!index_is_u32 && rsx_dsp_reg(rsx, M_RESTART_INDEX) > 0xFFFFu)
        return 0;
    return 1;
}

u32 rsx_dsp_restart_index(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_RESTART_INDEX);
}

void rsx_dsp_vertex_default(const rsx_dispatch* rsx, u32 index, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (index >= RSX_DSP_NUM_VERTEX_ATTR)
        return;
    u32 w[4];
    u32 any = 0;
    for (u32 i = 0; i < 4; i++) {
        w[i] = rsx_dsp_reg(rsx, M_VTX_ATTR_4F + index * 0x10 + i * 4);
        any |= w[i];
    }
    if (!any)
        return;   /* never written: hardware default (0,0,0,1) */
    memcpy(out, w, 16);
}

void rsx_dsp_get_index_array(const rsx_dispatch* rsx, rsx_dsp_index_array* out)
{
    out->offset   = rsx_dsp_reg(rsx, M_IDXBUF_OFFSET);
    const u32 fmt = rsx_dsp_reg(rsx, M_IDXBUF_FORMAT);
    out->location = fmt & 0xF;               /* location index directly     */
    out->is_u32   = ((fmt >> 4) & 0xFF) == 0; /* type 0 = u32, 1 = u16      */
}

/*
 * ps3recomp - RSX Vertex Program (NV40 ISA) -> HLSL decompiler
 *
 * See rsx_vp_decompiler.h. The 4-word instruction encoding (D0..D3 fields,
 * source operand layout, opcode numbers) follows the public NV40 vertex
 * program layout as documented in the NV_vertex_program3 extension spec and
 * the envytools rnndb; RPCS3 was consulted as a READ-ONLY semantics oracle
 * for the field positions and the dest-write rules (no code copied).
 *
 * Field facts used (verified against the oracle 2026-07-03):
 *   D0: mask_w/z/y/x pairs [2:9] are the CC swizzle, cond [10:12],
 *       cond_test_enable [13], dst_tmp [15:20], src0/1/2_abs [21:23],
 *       addr_reg_sel [24], saturate [26], vec_result [30]
 *   D1: src0h [0:7], input_src [8:11], const_src [12:21],
 *       vec_opcode [22:26], sca_opcode [27:31]
 *   D2: src2h [0:5], src1 [6:22], src0l [23:31]
 *   D3: end [0], index_const [1], dst [2:6], sca_dst_tmp [7:12],
 *       vec_writemask w/z/y/x [13:16], sca_writemask w/z/y/x [17:20],
 *       src2l [21:31]
 *   SRC (17 bits): reg_type [0:1] (1 = temp, 2 = input, 3 = constant),
 *       tmp_src [2:7], swz w/z/y/x [8:15] (2 bits each), neg [16]
 *   Dest rules: the VEC result goes to output o[D3.dst] when D0.vec_result
 *       (dst 0x1F = none) AND to temp r[D0.dst_tmp] (0x3F = none); the SCA
 *       result goes to o[D3.dst] when !D0.vec_result, else to
 *       r[D3.sca_dst_tmp]. The SCA unit reads SRC2.
 *   VEC ADD reads src0 + src2 (not src1).
 *   Output register map: o0 = HPOS, o1 = COL0, o2 = COL1, o3/o4 = back-face
 *       colors, o5.x = FOG, o6 = point size / TEX9, o7..o14 = TEX0..7,
 *       o15 = TEX8. Unwritten outputs default to (0,0,0,1).
 */
#include "rsx_vp_decompiler.h"
#include <stdio.h>
#include <string.h>

/* ---- little-endian word read (RPCS3-capture native order) --------------- */
static u32 rd_le(const u8* p)
{ return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24); }

/* ---- opcode names ------------------------------------------------------ */
const char* rsx_vp_vec_name(u32 op)
{
    static const char* n[] = {"NOP","MOV","MUL","ADD","MAD","DP3","DPH","DP4",
        "DST","MIN","MAX","SLT","SGE","ARL","FRC","FLR","SEQ","SFL","SGT","SLE",
        "SNE","STR","SSG","?","?","TXL"};
    return op < 26 ? n[op] : "?";
}
const char* rsx_vp_sca_name(u32 op)
{
    static const char* n[] = {"NOP","MOV","RCP","RCC","RSQ","EXP","LOG","LIT",
        "BRA","BRI","CAL","CLI","RET","LG2","EX2","SIN","COS","BRB","CLB","PSH","POP"};
    return op < 21 ? n[op] : "?";
}

u32 rsx_vp_program_size_instrs(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    u32 off = 0, instrs = 0;
    while (off + 16 <= max_bytes) {
        u32 d3 = rd_le(ucode + off + 12);
        instrs++;
        off += 16;
        if (d3 & 1u) return instrs;   /* D3.end */
    }
    return 0;
}

/* ---- output appender --------------------------------------------------- */
typedef struct { char* p; u32 cap; u32 len; int overflow; } Out;
static void emit(Out* o, const char* s)
{
    u32 n = (u32)strlen(s);
    if (o->len + n + 1 > o->cap) { o->overflow = 1; return; }
    memcpy(o->p + o->len, s, n); o->len += n; o->p[o->len] = '\0';
}

/* ---- source operand ---------------------------------------------------- */
static const char SWZ[4] = {'x','y','z','w'};

/* Build the HLSL expression for one 17-bit source field.
 * reg_type: 1 = temp r[], 2 = input v[], 3 = constant vp_c[]. */
static void emit_src(u32 src, int is_abs, u32 input_src, u32 const_src,
                     int index_const, u32 addr_sel, u32 addr_swz,
                     char* buf, u32 bufsz)
{
    u32 reg_type = src & 3;
    u32 tmp_src  = (src >> 2) & 0x3F;
    u32 sw = (src >> 8) & 3, sz = (src >> 10) & 3;
    u32 sy = (src >> 12) & 3, sx = (src >> 14) & 3;
    int neg = (src >> 16) & 1;

    char base[64];
    switch (reg_type) {
    case 2:  snprintf(base, sizeof(base), "v[%u]", input_src & 15); break;
    case 3:
        if (index_const)
            snprintf(base, sizeof(base), "vp_c[(%uu + a%u.%c) & 511u]",
                     const_src, addr_sel & 1, SWZ[addr_swz & 3]);
        else
            snprintf(base, sizeof(base), "vp_c[%u]", const_src & 511);
        break;
    default: snprintf(base, sizeof(base), "r[%u]", tmp_src & 31); break;
    }

    char swz[8];
    swz[0]='.'; swz[1]=SWZ[sx]; swz[2]=SWZ[sy]; swz[3]=SWZ[sz]; swz[4]=SWZ[sw]; swz[5]='\0';

    const char* pre = neg ? "-" : "";
    if (is_abs) snprintf(buf, bufsz, "%sabs(%s)%s", pre, base, swz);
    else        snprintf(buf, bufsz, "%s(%s)%s", pre, base, swz);
}

/* writemask string from 4 bits (x,y,z,w order) -> "xyzw" subset */
static void mask_str(int mx,int my,int mz,int mw, char* m)
{
    int i=0;
    if (mx) m[i++]='x';
    if (my) m[i++]='y';
    if (mz) m[i++]='z';
    if (mw) m[i++]='w';
    m[i]='\0';
}

/* Emit "    dst.mask = _v.mask;" (skips when mask empty). */
static void emit_store(Out* b, const char* dst_fmt, u32 idx, const char* m)
{
    if (!m[0]) return;
    char line[96];
    char dst[32];
    snprintf(dst, sizeof(dst), dst_fmt, idx);
    snprintf(line, sizeof(line), "        %s.%s = _v.%s;\n", dst, m, m);
    emit(b, line);
}

static u32 vec_source_use(u32 op, int* exact)
{
    switch (op) {
    case 0x00: return 0;       /* NOP */
    case 0x01: return 1u;      /* src0 */
    case 0x02: return 3u;      /* src0, src1 */
    case 0x03: return 5u;      /* src0, src2 */
    case 0x04: return 7u;      /* src0, src1, src2 */
    case 0x05: case 0x06: case 0x07: case 0x08:
    case 0x09: case 0x0A: case 0x0B: case 0x0C:
    case 0x10: case 0x12: case 0x13: case 0x14:
        return 3u;             /* src0, src1 */
    case 0x0D: case 0x0E: case 0x0F: case 0x16: case 0x19:
        return 1u;             /* src0 */
    case 0x11: case 0x15:
        return 0;              /* source-independent constants */
    default:
        *exact = 0;
        return 0;
    }
}

static u32 sca_source_use(u32 op, int* exact)
{
    switch (op) {
    case 0x00:
        return 0;
    case 0x01: case 0x02: case 0x03: case 0x04:
    case 0x05: case 0x06: case 0x07:
    case 0x0D: case 0x0E: case 0x0F: case 0x10:
        return 4u;             /* scalar ALU reads src2 */
    default:
        /* Flow control and currently unsupported scalar opcodes make a
         * narrowed data-flow result unsafe. */
        *exact = 0;
        return 0;
    }
}

int rsx_vp_analyze_inputs(const u8* ucode, u32 max_bytes,
                          rsx_vp_input_analysis* analysis)
{
    if (!ucode || !analysis) return -1;
    analysis->input_mask = 0;
    analysis->exact = 1;
    u32 off = 0;
    int instrs = 0;
    int ended = 0;
    while (off + 16 <= max_bytes) {
        const u32 d1 = rd_le(ucode + off + 4);
        const u32 d2 = rd_le(ucode + off + 8);
        const u32 d3 = rd_le(ucode + off + 12);
        off += 16;
        instrs++;

        const u32 vec_op = (d1 >> 22) & 0x1F;
        const u32 sca_op = (d1 >> 27) & 0x1F;
        const u32 input_src = (d1 >> 8) & 0xF;
        const u32 src0 = ((d2 >> 23) & 0x1FF) | ((d1 & 0xFF) << 9);
        const u32 src1 = (d2 >> 6) & 0x1FFFF;
        const u32 src2 = ((d3 >> 21) & 0x7FF) | ((d2 & 0x3F) << 11);
        const u32 sources[3] = {src0, src1, src2};
        const int vec_writes = ((d3 >> 13) & 0xFu) != 0;
        const int sca_writes = ((d3 >> 17) & 0xFu) != 0;
        u32 use = 0;
        if (vec_writes && vec_op)
            use |= vec_source_use(vec_op, &analysis->exact);
        if (sca_writes && sca_op)
            use |= sca_source_use(sca_op, &analysis->exact);
        for (u32 source = 0; source < 3; source++)
            if ((use & (1u << source)) && (sources[source] & 3u) == 2u)
                analysis->input_mask |= 1u << input_src;

        if (d3 & 1u) {
            ended = 1;
            break;
        }
    }
    if (!ended) {
        analysis->input_mask = 0xFFFFu;
        analysis->exact = 0;
        return -1;
    }
    if (!analysis->exact)
        analysis->input_mask = 0xFFFFu;
    return instrs;
}

static int rsx_vp_decompile_impl(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, int compact_inputs,
    u32 input_mask, char* out, u32 out_size);

int rsx_vp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size)
{
    return rsx_vp_decompile_ex(ucode, max_bytes, 0u, out, out_size);
}

int rsx_vp_decompile_ex(const u8* ucode, u32 max_bytes, u32 vtex_mask,
                        char* out, u32 out_size)
{
    return rsx_vp_decompile_impl(
        ucode, max_bytes, vtex_mask, 0, 0xFFFFu, out, out_size);
}

int rsx_vp_decompile_compact_ex(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, u32 input_mask,
    char* out, u32 out_size)
{
    rsx_vp_input_analysis analysis;
    if (rsx_vp_analyze_inputs(ucode, max_bytes, &analysis) < 0 ||
        !analysis.exact ||
        (analysis.input_mask & ~(input_mask & 0xFFFFu)) != 0)
        input_mask = 0xFFFFu;
    else
        input_mask = analysis.input_mask;
    return rsx_vp_decompile_impl(
        ucode, max_bytes, vtex_mask, 1, input_mask, out, out_size);
}

static int rsx_vp_decompile_impl(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, int compact_inputs,
    u32 input_mask, char* out, u32 out_size)
{
    if (!ucode || !out || out_size < 256) return -1;

    /* Body is built first, preamble/epilogue wrap it. */
    static char body[192 * 1024];
    Out b = { body, sizeof(body), 0, 0 };
    body[0] = '\0';

    int n_cond_skipped = 0, n_flow_skipped = 0;

    u32 off = 0; int instrs = 0;
    while (off + 16 <= max_bytes) {
        u32 d0 = rd_le(ucode+off+0), d1 = rd_le(ucode+off+4);
        u32 d2 = rd_le(ucode+off+8), d3 = rd_le(ucode+off+12);
        off += 16; instrs++;

        u32 vec_op = (d1 >> 22) & 0x1F;
        u32 sca_op = (d1 >> 27) & 0x1F;
        u32 input_src = (d1 >> 8) & 0xF;
        u32 const_src = (d1 >> 12) & 0x3FF;
        int index_const = (d3 >> 1) & 1;

        /* assemble the three 17-bit source fields */
        u32 src0 = ((d2 >> 23) & 0x1FF) | ((d1 & 0xFF) << 9);
        u32 src1 = (d2 >> 6) & 0x1FFFF;
        u32 src2 = ((d3 >> 21) & 0x7FF) | ((d2 & 0x3F) << 11);
        int a0 = (d0 >> 21) & 1, a1 = (d0 >> 22) & 1, a2 = (d0 >> 23) & 1;
        u32 addr_sel = (d0 >> 24) & 1;
        u32 addr_swz = d0 & 3;
        int saturate = (d0 >> 26) & 1;
        int cond_test = (d0 >> 13) & 1;
        u32 cond = (d0 >> 10) & 7;

        char A[96], B[96], C[96];
        emit_src(src0, a0, input_src, const_src, index_const, addr_sel, addr_swz, A, sizeof(A));
        emit_src(src1, a1, input_src, const_src, index_const, addr_sel, addr_swz, B, sizeof(B));
        emit_src(src2, a2, input_src, const_src, index_const, addr_sel, addr_swz, C, sizeof(C));

        u32 dst_tmp     = (d0 >> 15) & 0x3F;
        u32 dst_out     = (d3 >> 2) & 0x1F;
        u32 sca_dst_tmp = (d3 >> 7) & 0x3F;
        int vec_result  = (d0 >> 30) & 1;

        int vmx=(d3>>16)&1, vmy=(d3>>15)&1, vmz=(d3>>14)&1, vmw=(d3>>13)&1;
        int smx=(d3>>20)&1, smy=(d3>>19)&1, smz=(d3>>18)&1, smw=(d3>>17)&1;

        /* Condition-code tests are not modeled (no CC register file yet):
         * cond==7 (always) and cond_test disabled execute normally, anything
         * else is emitted unconditionally with a marker. */
        if (cond_test && cond != 7) {
            n_cond_skipped++;
            emit(&b, "    /* WARNING: cond-test on next op not modeled */\n");
        }

        /* ---- vector ALU ---- */
        if ((vmx|vmy|vmz|vmw) && vec_op != 0x00) {
            char rhs[512];
            int handled = 1;
            switch (vec_op) {
            case 0x01: snprintf(rhs,sizeof rhs,"%s",A); break;                 /* MOV */
            case 0x02: snprintf(rhs,sizeof rhs,"(%s)*(%s)",A,B); break;        /* MUL */
            case 0x03: snprintf(rhs,sizeof rhs,"(%s)+(%s)",A,C); break;        /* ADD: src0 + src2 */
            case 0x04: snprintf(rhs,sizeof rhs,"(%s)*(%s)+(%s)",A,B,C); break; /* MAD */
            case 0x05: snprintf(rhs,sizeof rhs,"dot((%s).xyz,(%s).xyz)",A,B); break; /* DP3 */
            case 0x06: snprintf(rhs,sizeof rhs,"(dot((%s).xyz,(%s).xyz)+(%s).w)",A,B,B); break; /* DPH */
            case 0x07: snprintf(rhs,sizeof rhs,"dot((%s),(%s))",A,B); break; /* DP4 */
            case 0x08: snprintf(rhs,sizeof rhs,"float4(1,(%s).y*(%s).y,(%s).z,(%s).w)",A,B,A,B); break; /* DST */
            case 0x09: snprintf(rhs,sizeof rhs,"min((%s),(%s))",A,B); break;   /* MIN */
            case 0x0A: snprintf(rhs,sizeof rhs,"max((%s),(%s))",A,B); break;   /* MAX */
            case 0x0B: snprintf(rhs,sizeof rhs,"(float4)((%s)<(%s))",A,B); break;  /* SLT */
            case 0x0C: snprintf(rhs,sizeof rhs,"(float4)((%s)>=(%s))",A,B); break; /* SGE */
            case 0x0E: snprintf(rhs,sizeof rhs,"frac(%s)",A); break;           /* FRC */
            case 0x0F: snprintf(rhs,sizeof rhs,"floor(%s)",A); break;          /* FLR */
            case 0x10: snprintf(rhs,sizeof rhs,"(float4)((%s)==(%s))",A,B); break; /* SEQ */
            case 0x11: snprintf(rhs,sizeof rhs,"(float4)0"); break;            /* SFL */
            case 0x12: snprintf(rhs,sizeof rhs,"(float4)((%s)>(%s))",A,B); break;  /* SGT */
            case 0x13: snprintf(rhs,sizeof rhs,"(float4)((%s)<=(%s))",A,B); break; /* SLE */
            case 0x14: snprintf(rhs,sizeof rhs,"(float4)((%s)!=(%s))",A,B); break; /* SNE */
            case 0x15: snprintf(rhs,sizeof rhs,"float4(1,1,1,1)"); break;      /* STR */
            case 0x16: snprintf(rhs,sizeof rhs,"sign(%s)",A); break;           /* SSG */
            case 0x19: {
                const u32 tex_unit = (d2 >> 8) & 3u;
                if ((vtex_mask >> tex_unit) & 1u)
                    snprintf(rhs, sizeof rhs,
                        "rsx_vtex%u.SampleLevel(rsx_vsamp%u, (%s).xy, 0.0)",
                        tex_unit, tex_unit, A);
                else
                    snprintf(rhs, sizeof rhs, "float4(0,0,0,0)");
                break;
            }
            case 0x0D: /* ARL: address register load, rounds down */
                if (vmx|vmy|vmz|vmw) {
                    char m[6]; mask_str(vmx,vmy,vmz,vmw,m);
                    char line[256];
                    snprintf(line,sizeof line,
                        "    { int4 _a = (int4)floor(%s); a%u.%s = _a.%s; }\n",
                        A, addr_sel, m, m);
                    emit(&b, line);
                }
                handled = 0;
                break;
            default:
                n_flow_skipped++;
                emit(&b, "    /* TODO: unhandled VP vec op */\n");
                handled = 0;
                break;
            }

            if (handled) {
                char m[6]; mask_str(vmx,vmy,vmz,vmw,m);
                char line[600];
                snprintf(line,sizeof line,"    { float4 _v = (float4)(%s);%s\n",
                         rhs, saturate ? " _v = saturate(_v);" : "");
                emit(&b, line);
                if (vec_result && dst_out != 0x1F)
                    emit_store(&b, "o[%u]", dst_out & 15, m);
                if (dst_tmp != 0x3F)
                    emit_store(&b, "r[%u]", dst_tmp & 31, m);
                if (!vec_result && dst_tmp == 0x3F)
                    emit(&b, "        /* TODO: CC-only write not modeled */\n");
                emit(&b, "    }\n");
            }
        }

        /* ---- scalar ALU (reads SRC2) ---- */
        if ((smx|smy|smz|smw) && sca_op != 0x00) {
            char rhs[256];
            switch (sca_op) {
            case 0x01: snprintf(rhs,sizeof rhs,"%s",C); break;                /* MOV */
            case 0x02: snprintf(rhs,sizeof rhs,"(1.0/(%s).x)",C); break;      /* RCP */
            case 0x03: snprintf(rhs,sizeof rhs,"clamp(1.0/(%s).x, 5.42101e-20, 1.884467e19)",C); break; /* RCC */
            case 0x04: snprintf(rhs,sizeof rhs,"rsqrt(max(abs((%s).x), 1e-10))",C); break; /* RSQ */
            case 0x05: snprintf(rhs,sizeof rhs,"exp2((%s).x)",C); break;      /* EXP */
            case 0x06: snprintf(rhs,sizeof rhs,"log2(max(abs((%s).x), 1e-10))",C); break; /* LOG */
            case 0x0D: snprintf(rhs,sizeof rhs,"log2(max(abs((%s).x), 1e-10))",C); break; /* LG2 */
            case 0x0E: snprintf(rhs,sizeof rhs,"exp2((%s).x)",C); break;      /* EX2 */
            case 0x0F: snprintf(rhs,sizeof rhs,"sin((%s).x)",C); break;       /* SIN */
            case 0x10: snprintf(rhs,sizeof rhs,"cos((%s).x)",C); break;       /* COS */
            case 0x07: /* LIT: (1, max(s.x,0), s.x>0 ? 2^(s.w*log2(max(s.y,eps))) : 0, 1) */
                snprintf(rhs,sizeof rhs,
                    "float4(1.0, max((%s).x, 0.0), ((%s).x > 0.0) ? "
                    "exp2(clamp((%s).w, -127.996, 127.996) * log2(max((%s).y, 1e-10))) : 0.0, 1.0)",
                    C, C, C, C);
                break;
            default:
                n_flow_skipped++;
                emit(&b, "    /* TODO: VP flow-control op skipped */\n");
                rhs[0]='\0';
                break;
            }
            if (rhs[0]) {
                char m[6]; mask_str(smx,smy,smz,smw,m);
                char line[600];
                snprintf(line,sizeof line,"    { float4 _v = (float4)(%s);%s\n",
                         rhs, saturate ? " _v = saturate(_v);" : "");
                emit(&b, line);
                /* SCA writes the output register when the VEC unit does not
                 * own it; otherwise it targets its own temp. */
                if (!vec_result && dst_out != 0x1F)
                    emit_store(&b, "o[%u]", dst_out & 15, m);
                if (sca_dst_tmp != 0x3F)
                    emit_store(&b, "r[%u]", sca_dst_tmp & 31, m);
                emit(&b, "    }\n");
            }
        }

        if (d3 & 1u) break; /* end */
    }

    if (b.overflow) return -1;

    /* ---- assemble full shader ---- */
    Out o = { out, out_size, 0, 0 };
    if (!compact_inputs) {
        emit(&o,
            /* All 16 RSX vertex attributes arrive as float4 (the harness
             * fetches and converts them on the CPU). */
            "struct VSInput {\n"
            "    float4 a0:ATTR0;  float4 a1:ATTR1;  float4 a2:ATTR2;  float4 a3:ATTR3;\n"
            "    float4 a4:ATTR4;  float4 a5:ATTR5;  float4 a6:ATTR6;  float4 a7:ATTR7;\n"
            "    float4 a8:ATTR8;  float4 a9:ATTR9;  float4 a10:ATTR10; float4 a11:ATTR11;\n"
            "    float4 a12:ATTR12; float4 a13:ATTR13; float4 a14:ATTR14; float4 a15:ATTR15;\n"
            "};\n");
    } else {
        emit(&o, "struct VSInput {\n");
        if (!(input_mask & 0xFFFFu))
            emit(&o, "    uint vertex_id:SV_VertexID;\n");
        for (u32 attr = 0; attr < 16; attr++) {
            if (!(input_mask & (1u << attr))) continue;
            char declaration[64];
            snprintf(declaration, sizeof(declaration),
                     "    float4 a%u:ATTR%u;\n", attr, attr);
            emit(&o, declaration);
        }
        emit(&o, "};\n");
    }
    emit(&o,
        "struct VSOutput {\n"
        "    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
        "    float4 fog:FOG;\n"
        "    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
        "};\n"
        /* b0: 512 vec4 transform constants + the RSX viewport transform,
         * pre-mapped by the harness to D3D clip space:
         *   ndc.xyz = clip.xyz * vp_posscale.xyz + clip.w * vp_posoffset.xyz */
        "cbuffer VPConst : register(b0) {\n"
        "    float4 vp_c[512];\n"
        "    float4 vp_posscale;\n"
        "    float4 vp_posoffset;\n"
        "};\n");

    for (u32 vtu = 0; vtu < 4; vtu++) {
        if (!((vtex_mask >> vtu) & 1u)) continue;
        char decl[128];
        snprintf(decl, sizeof(decl),
            "Texture2D rsx_vtex%u : register(t%u);\n"
            "SamplerState rsx_vsamp%u : register(s%u);\n",
            vtu, 16u + vtu, vtu, vtu);
        emit(&o, decl);
    }

    emit(&o, "VSOutput main(VSInput input) {\n");
    if (!compact_inputs) {
        emit(&o,
            "    float4 v[16];\n"
            "    v[0]=input.a0;  v[1]=input.a1;  v[2]=input.a2;  v[3]=input.a3;\n"
            "    v[4]=input.a4;  v[5]=input.a5;  v[6]=input.a6;  v[7]=input.a7;\n"
            "    v[8]=input.a8;  v[9]=input.a9;  v[10]=input.a10; v[11]=input.a11;\n"
            "    v[12]=input.a12; v[13]=input.a13; v[14]=input.a14; v[15]=input.a15;\n");
    } else {
        emit(&o,
            "    float4 v[16];\n"
            "    [unroll] for (int _k=0;_k<16;_k++) v[_k]=float4(0,0,0,1);\n");
        for (u32 attr = 0; attr < 16; attr++) {
            if (!(input_mask & (1u << attr))) continue;
            char assignment[64];
            snprintf(assignment, sizeof(assignment),
                     "    v[%u]=input.a%u;\n", attr, attr);
            emit(&o, assignment);
        }
    }
    emit(&o,
        "    float4 r[32]; float4 o[16];\n"
        "    [unroll] for (int _i=0;_i<32;_i++) r[_i]=(float4)0;\n"
        "    [unroll] for (int _j=0;_j<16;_j++) o[_j]=float4(0,0,0,1);\n"
        "    int4 a0 = (int4)0; int4 a1 = (int4)0;\n");

    emit(&o, body);

    /* Output register map (see header comment): o0 = HPOS through the RSX
     * viewport transform, o1/o2 = colors, o5.x = fog, o7..o14 = TEX0..7. */
    emit(&o,
        "    VSOutput Out;\n"
        "    float4 _p = o[0];\n"
        "    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n"
        "    Out.col0 = o[1]; Out.col1 = o[2]; Out.fog = o[5].xxxx;\n"
        "    Out.t0=o[7];  Out.t1=o[8];  Out.t2=o[9];  Out.t3=o[10];\n"
        "    Out.t4=o[11]; Out.t5=o[12]; Out.t6=o[13]; Out.t7=o[14];\n"
        "    return Out;\n"
        "}\n");

    (void)n_cond_skipped;
    (void)n_flow_skipped;
    if (o.overflow) return -1;
    return instrs;
}

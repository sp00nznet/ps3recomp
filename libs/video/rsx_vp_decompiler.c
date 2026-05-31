/*
 * ps3recomp - RSX Vertex Program (NV40 ISA) -> HLSL decompiler
 * See rsx_vp_decompiler.h. Encoding from RPCS3 RSXVertexProgram.h (D0..D3).
 */
#include "rsx_vp_decompiler.h"
#include <stdio.h>
#include <string.h>

/* ---- little-endian word read (RPCS3 native order) ---------------------- */
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

/* Build the HLSL expression for one 17-bit source field. */
static void emit_src(u32 src, int is_abs, u32 input_src, u32 const_src,
                     char* buf, u32 bufsz)
{
    u32 reg_type = src & 3;
    u32 tmp_src  = (src >> 2) & 0x3F;
    u32 sx = (src >> 14) & 3, sy = (src >> 12) & 3;
    u32 sz = (src >> 10) & 3, sw = (src >> 8) & 3;
    int neg = (src >> 16) & 1;

    char base[32];
    switch (reg_type) {
    case 1:  snprintf(base, sizeof(base), "v[%u]", input_src); break;  /* input attr */
    case 2:  snprintf(base, sizeof(base), "vp_c[%u]", const_src); break; /* constant */
    default: snprintf(base, sizeof(base), "r[%u]", tmp_src); break;    /* temp */
    }

    char swz[8];
    swz[0]='.'; swz[1]=SWZ[sx]; swz[2]=SWZ[sy]; swz[3]=SWZ[sz]; swz[4]=SWZ[sw]; swz[5]='\0';

    const char* pre = neg ? "-" : "";
    if (is_abs) snprintf(buf, bufsz, "%sabs(%s)%s", pre, base, swz);
    else        snprintf(buf, bufsz, "%s(%s)%s", pre, base, swz);
}

/* writemask string from 4 bits (x,y,z,w order) -> ".xyzw" subset */
static void mask_str(int mx,int my,int mz,int mw, char* m)
{
    int i=0;
    m[i++]='.';
    if (mx) m[i++]='x';
    if (my) m[i++]='y';
    if (mz) m[i++]='z';
    if (mw) m[i++]='w';
    if (i==1) m[0]='\0'; else m[i]='\0';   /* empty if no mask */
}

int rsx_vp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size)
{
    if (!ucode || !out || out_size < 256) return -1;

    /* Body is built first, preamble/epilogue wrap it. */
    static char body[96 * 1024];
    Out b = { body, sizeof(body), 0, 0 };
    body[0] = '\0';

    u32 off = 0; int instrs = 0;
    while (off + 16 <= max_bytes) {
        u32 d0 = rd_le(ucode+off+0), d1 = rd_le(ucode+off+4);
        u32 d2 = rd_le(ucode+off+8), d3 = rd_le(ucode+off+12);
        off += 16; instrs++;

        u32 vec_op = (d1 >> 22) & 0x1F;
        u32 sca_op = (d1 >> 27) & 0x1F;
        u32 input_src = (d1 >> 8) & 0xF;
        u32 const_src = (d1 >> 12) & 0x3FF;

        /* assemble the three 17-bit source fields */
        u32 src0 = ((d2 >> 23) & 0x1FF) | ((d1 & 0xFF) << 9);
        u32 src1 = (d2 >> 6) & 0x1FFFF;
        u32 src2 = ((d3 >> 21) & 0x7FF) | ((d2 & 0x3F) << 11);
        int a0 = (d0 >> 21) & 1, a1 = (d0 >> 22) & 1, a2 = (d0 >> 23) & 1;

        char A[64], B[64], C[64];
        emit_src(src0, a0, input_src, const_src, A, sizeof(A));
        emit_src(src1, a1, input_src, const_src, B, sizeof(B));
        emit_src(src2, a2, input_src, const_src, C, sizeof(C));

        u32 dst_tmp     = (d0 >> 15) & 0x3F;
        u32 dst_out     = (d3 >> 2) & 0x1F;
        u32 sca_dst_tmp = (d3 >> 7) & 0x3F;
        int vec_result  = (d0 >> 30) & 1;

        int vmx=(d3>>16)&1, vmy=(d3>>15)&1, vmz=(d3>>14)&1, vmw=(d3>>13)&1;
        int smx=(d3>>20)&1, smy=(d3>>19)&1, smz=(d3>>18)&1, smw=(d3>>17)&1;

        /* ---- vector ALU ---- */
        if ((vmx|vmy|vmz|vmw) && vec_op != 0x00) {
            char rhs[384];
            switch (vec_op) {
            case 0x01: snprintf(rhs,sizeof rhs,"%s",A); break;                 /* MOV */
            case 0x02: snprintf(rhs,sizeof rhs,"(%s)*(%s)",A,B); break;        /* MUL */
            case 0x03: snprintf(rhs,sizeof rhs,"(%s)+(%s)",A,B); break;        /* ADD */
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
            case 0x12: snprintf(rhs,sizeof rhs,"(float4)((%s)>(%s))",A,B); break;  /* SGT */
            case 0x13: snprintf(rhs,sizeof rhs,"(float4)((%s)<=(%s))",A,B); break; /* SLE */
            case 0x14: snprintf(rhs,sizeof rhs,"(float4)((%s)!=(%s))",A,B); break; /* SNE */
            case 0x16: snprintf(rhs,sizeof rhs,"sign(%s)",A); break;           /* SSG */
            case 0x0D: snprintf(rhs,sizeof rhs,"floor(%s)",A); break;          /* ARL (addr) ~ floor */
            default:   snprintf(rhs,sizeof rhs,"%s",A); break;  /* SFL/STR/TXL/unknown: passthrough */
            }
            char m[6]; mask_str(vmx,vmy,vmz,vmw,m);
            char line[512];
            const char* tgt = vec_result ? "o" : "r";
            u32 idx = vec_result ? dst_out : dst_tmp;
            snprintf(line,sizeof line,"    %s[%u]%s = (%s);\n", tgt, idx, m, rhs);
            emit(&b, line);
        }

        /* ---- scalar ALU ---- */
        if ((smx|smy|smz|smw) && sca_op != 0x00) {
            char rhs[256];
            switch (sca_op) {
            case 0x01: snprintf(rhs,sizeof rhs,"%s",A); break;            /* MOV */
            case 0x02: snprintf(rhs,sizeof rhs,"(1.0/(%s).x)",A); break;  /* RCP */
            case 0x03: snprintf(rhs,sizeof rhs,"(1.0/(%s).x)",A); break;  /* RCC ~ RCP */
            case 0x04: snprintf(rhs,sizeof rhs,"rsqrt((%s).x)",A); break; /* RSQ */
            case 0x05: snprintf(rhs,sizeof rhs,"exp2((%s).x)",A); break;  /* EXP */
            case 0x06: snprintf(rhs,sizeof rhs,"log2((%s).x)",A); break;  /* LOG */
            case 0x0D: snprintf(rhs,sizeof rhs,"log2((%s).x)",A); break;  /* LG2 */
            case 0x0E: snprintf(rhs,sizeof rhs,"exp2((%s).x)",A); break;  /* EX2 */
            case 0x0F: snprintf(rhs,sizeof rhs,"sin((%s).x)",A); break;   /* SIN */
            case 0x10: snprintf(rhs,sizeof rhs,"cos((%s).x)",A); break;   /* COS */
            case 0x07: snprintf(rhs,sizeof rhs,"%s",A); break;            /* LIT ~ passthrough */
            default:   rhs[0]='\0'; break; /* BRA/CAL/RET/flow: no result */
            }
            if (rhs[0]) {
                char m[6]; mask_str(smx,smy,smz,smw,m);
                /* sca writes the output when the vec ALU isn't producing it. */
                const char* tgt = vec_result ? "r" : "o";
                u32 idx = vec_result ? sca_dst_tmp : dst_out;
                char line[512];
                snprintf(line,sizeof line,"    %s[%u]%s = (%s);\n", tgt, idx, m, rhs);
                emit(&b, line);
                /* also keep a temp copy so later instructions can read it */
                char line2[512];
                snprintf(line2,sizeof line2,"    r[%u]%s = (%s);\n", sca_dst_tmp, m, rhs);
                emit(&b, line2);
            }
        }

        if (d3 & 1u) break; /* end */
    }

    if (b.overflow) return -1;

    /* ---- assemble full shader ---- */
    Out o = { out, out_size, 0, 0 };
    emit(&o,
        /* Input layout is intentionally minimal so it matches the backend's
         * existing vertex feed (POSITION float3 + COLOR0 float4); the other
         * 14 attribute slots are zero locals. Generalising to all 16 RSX
         * vertex attributes is a follow-up (needs a wide vertex + layout). */
        "struct VSInput {\n"
        "    float3 v0 : POSITION;\n"
        "    float4 v3 : COLOR0;\n"
        "};\n"
        "struct VSOutput {\n"
        "    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
        "    float4 fog:FOG;\n"
        "    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
        "};\n"
        /* vp_c sized to the full 9-bit-plus const bank; r/o sized to the full
         * temp(6-bit)/output(5-bit) index fields so no constant index can be
         * out of bounds (HLSL error X3504). */
        "cbuffer VPConst : register(b0) { float4 vp_c[1024]; };\n"
        "VSOutput main(VSInput input) {\n"
        "    float4 v[16];\n"
        "    [unroll] for (int _k=0;_k<16;_k++) v[_k]=(float4)0;\n"
        "    v[0]=float4(input.v0,1); v[3]=input.v3;\n"
        "    float4 r[64]; float4 o[32];\n"
        "    [unroll] for (int _i=0;_i<64;_i++) r[_i]=(float4)0;\n"
        "    [unroll] for (int _j=0;_j<32;_j++) o[_j]=(float4)0;\n");

    emit(&o, body);

    /* Map NV40 VP output registers to the VSOutput varyings.
     * o[0]=HPOS, o[1]=COL0, o[2]=COL1, o[5]=FOG, o[8..15]=TEX0..7. */
    emit(&o,
        "    VSOutput Out;\n"
        "    Out.pos = o[0]; Out.col0 = o[1]; Out.col1 = o[2]; Out.fog = o[5];\n"
        "    Out.t0=o[8]; Out.t1=o[9]; Out.t2=o[10]; Out.t3=o[11];\n"
        "    Out.t4=o[12]; Out.t5=o[13]; Out.t6=o[14]; Out.t7=o[15];\n"
        "    return Out;\n"
        "}\n");

    if (o.overflow) return -1;
    return instrs;
}

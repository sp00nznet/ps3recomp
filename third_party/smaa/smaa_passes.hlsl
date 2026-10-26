cbuffer SmaaParams : register(b0) { float4 rtMetrics; };
#define SMAA_RT_METRICS rtMetrics
#define SMAA_HLSL_4 1
#define SMAA_PRESET_HIGH 1
#include "SMAA.hlsl"

Texture2D colorTex  : register(t0);
Texture2D edgesTex  : register(t1);
Texture2D areaTex   : register(t2);
Texture2D searchTex : register(t3);
Texture2D blendTex  : register(t4);

struct V1 { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 o0 : TEXCOORD1; float4 o1 : TEXCOORD2; float4 o2 : TEXCOORD3; };
struct V2 { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o0 : TEXCOORD2; float4 o1 : TEXCOORD3; float4 o2 : TEXCOORD4; };
struct V3 { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 o0 : TEXCOORD1; };

void fs_tri(uint id, out float4 pos, out float2 uv)
{
    float2 p = float2((id << 1) & 2, id & 2);
    pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    uv = float2(p.x, 1.0 - p.y);
}

V1 smaa_edge_vs(uint id : SV_VertexID)
{
    V1 o; fs_tri(id, o.pos, o.uv);
    float4 off[3]; SMAAEdgeDetectionVS(o.uv, off);
    o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2]; return o;
}
float4 smaa_edge_ps(V1 i) : SV_Target
{
    float4 off[3] = { i.o0, i.o1, i.o2 };
    return float4(SMAALumaEdgeDetectionPS(i.uv, off, colorTex), 0.0, 0.0);
}
V2 smaa_weight_vs(uint id : SV_VertexID)
{
    V2 o; fs_tri(id, o.pos, o.uv);
    float4 off[3]; SMAABlendingWeightCalculationVS(o.uv, o.pix, off);
    o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2]; return o;
}
float4 smaa_weight_ps(V2 i) : SV_Target
{
    float4 off[3] = { i.o0, i.o1, i.o2 };
    return SMAABlendingWeightCalculationPS(i.uv, i.pix, off, edgesTex, areaTex, searchTex, float4(0, 0, 0, 0));
}
V3 smaa_blend_vs(uint id : SV_VertexID)
{
    V3 o; fs_tri(id, o.pos, o.uv);
    float4 off; SMAANeighborhoodBlendingVS(o.uv, off); o.o0 = off; return o;
}
float4 smaa_blend_ps(V3 i) : SV_Target
{
    return SMAANeighborhoodBlendingPS(i.uv, i.o0, colorTex, blendTex);
}

#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <assert.h>
#include <stdio.h>
#include "rsx_d3d12_point_quad.h"
#pragma comment(lib,"d3dcompiler.lib")
int main(void) {
 const char* source=
 "struct VSInput {float4 a0:ATTR0;};\n"
 "struct VSOutput {float4 pos:SV_Position; float4 t0:TEXCOORD0; float4 t4:TEXCOORD4;};\n"
 "cbuffer VPConst : register(b0) {float4 vp_c[512]; float4 vp_posscale;\n    float4 vp_posoffset;\n};\n"
 "VSOutput main(VSInput input) { float4 o[16]; o[0]=input.a0; o[6]=4;\n"
 "VSOutput Out; Out.pos=o[0]; Out.t0=1; Out.t4=0;\n    return Out;\n}\n";
 char text[8192],original[8192];strcpy(text,source);strcpy(original,source);
 unsigned key=rsx_d3d12_point_key(1,0x1001);
 assert(key==(0x40000000u|16));
 assert(rsx_d3d12_point_quad_patch(text,strlen(text)+1,key)==-1);
 assert(!strcmp(text,original));
 assert(rsx_d3d12_point_quad_patch(text,sizeof text,0)==0);
 assert(rsx_d3d12_point_quad_patch(text,sizeof text,key)==1);
 assert(strstr(text,"Out.t4=float4(_uv,0,1)"));
 assert(!strstr(text,"Out.t0=float4(_uv,0,1)"));
 assert(strstr(text,"_corner[_d3d_point_vertex%6u]"));
 assert(!strstr(text,"_d3d_point_vertex-"));
 assert(strstr(text,"d3d_point_view.xy"));
 strcpy(original,text);
 assert(rsx_d3d12_point_quad_patch(text,sizeof text,key)==0 && !strcmp(text,original));
 ID3DBlob *shader=NULL,*error=NULL;
 HRESULT hr=D3DCompile(text,strlen(text),"point_quad_test",NULL,NULL,"main","vs_5_0",0,0,&shader,&error);
 if(FAILED(hr))fprintf(stderr,"%s\n",error?(const char*)ID3D10Blob_GetBufferPointer(error):"D3DCompile failure");
 assert(SUCCEEDED(hr) && shader);
 if(error)ID3D10Blob_Release(error);if(shader)ID3D10Blob_Release(shader);
 puts("PASS selected TC4, draw-relative vertex IDs, separate viewport axes, atomic capacity, idempotence and actual D3DCompile VS");
 return 0;
}

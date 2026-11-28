/* Synthetic long program: all observable work lies after the old 4 KiB cap. */
#include "rsx_fp_decompiler.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include "rsx_d3d12_fp_size.h"
#include <d3dcompiler.h>
#pragma comment(lib,"d3dcompiler.lib")
#endif
#ifndef RSX_D3D12_FP_MAX_BYTES
#define RSX_D3D12_FP_MAX_BYTES 0x10000u
#endif
static void word(u8*p,u32 v){u32 b=(v<<16)|(v>>16);p[0]=b>>24;p[1]=b>>16;p[2]=b>>8;p[3]=b;}
static void ins(u8*p,u32 a,u32 b,u32 c,u32 d){word(p,a);word(p+4,b);word(p+8,c);word(p+12,d);}
int main(void)
{
    static u8 code[RSX_D3D12_FP_MAX_BYTES];
    static char hlsl[RSX_FP_HLSL_BUFFER_SIZE];
    const u32 swz=(1u<<11)|(2u<<13)|(3u<<15);
    const u32 always=(7u<<18)|(1u<<23)|(2u<<25)|(3u<<27);
    for(u32 i=0;i<260;i++) ins(code+i*16,(1u<<24)|(15u<<9),swz|always,0,0);
    const u32 tail=260*16;
    ins(code+tail,(0x17u<<24)|(15u<<9)|(9u<<17)|(4u<<13),1|swz|always,0,0);
    ins(code+tail+16,(2u<<24)|(15u<<9)|1,swz|always,2|swz,0);
    word(code+tail+32,0x3e800000);word(code+tail+36,0x3f000000);
    word(code+tail+40,0x3f400000);word(code+tail+44,0x3f800000);
    const u32 size=tail+48;
    assert(rsx_fp_program_size(code,4096)==0);
    assert(rsx_fp_program_size(code,sizeof code)==size);
    assert(rsx_fp_program_size(code,size-1)==0);
    assert(rsx_fp_decompile(code,size,0x40,hlsl,sizeof hlsl)==262);
    assert(strstr(hlsl,"rsx_tex[9].Sample"));
    assert(rsx_fp_texture_units(code,size)==(1u<<9));
    float constants[4]={0};assert(rsx_fp_extract_consts(code,size,constants,1)==1);
    assert(constants[0]==.25f && constants[1]==.5f && constants[2]==.75f && constants[3]==1);
    const u32 hash=rsx_fp_code_hash(code,size);
    word(code+tail,(0x17u<<24)|(15u<<9)|(10u<<17)|(4u<<13));
    assert(hash!=rsx_fp_code_hash(code,size));
    word(code+tail,(0x17u<<24)|(15u<<9)|(9u<<17)|(4u<<13));
    word(code+tail+32,0x3f000000);assert(hash==rsx_fp_code_hash(code,size));
    word(code+tail+32,0x3e800000);
#ifdef _WIN32
    /* Exercise production sizing against a committed region followed by
     * inaccessible memory: no END must not cause an out-of-bounds read. */
    u8* guarded = VirtualAlloc(NULL, 0x20000, MEM_RESERVE, PAGE_NOACCESS);
    assert(guarded && VirtualAlloc(guarded, 8192, MEM_COMMIT, PAGE_READWRITE));
    memcpy(guarded,code,size);
    assert(d3d12_fp_program_size(guarded)==size);
    memset(guarded,0,8192);
    assert(d3d12_fp_program_size(guarded)==0);
    ins(guarded,(1u<<24)|(15u<<9)|1,swz|always,0,0);
    assert(d3d12_fp_program_size(guarded)==16);
    assert(VirtualFree(guarded,0,MEM_RELEASE));
    ID3DBlob*blob=NULL,*errors=NULL;
    HRESULT hr=D3DCompile(hlsl,strlen(hlsl),"long-fp",NULL,NULL,"main","ps_5_0",0,0,&blob,&errors);
    if(FAILED(hr)&&errors)fprintf(stderr,"%s\n",(char*)errors->lpVtbl->GetBufferPointer(errors));
    assert(SUCCEEDED(hr)&&blob);blob->lpVtbl->Release(blob);if(errors)errors->lpVtbl->Release(errors);
#endif
    puts("PASS long FP END bounds, tail sampling/constants/hash (Windows also checks guarded memory and real HLSL)");
    return 0;
}

// Real D3D12 regression: point-quad output must be independent of the draw's
// StartVertexLocation. WARP keeps the fixture headless and deterministic.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "rsx_d3d12_point_quad.h"
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
#define OK(x) do { HRESULT hr=(x); if(FAILED(hr)){fprintf(stderr,"HRESULT %08lX line %d\n",hr,__LINE__);return 1;} } while(0)
int main(){
 ComPtr<IDXGIFactory4> factory;OK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
 ComPtr<IDXGIAdapter> adapter;OK(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
 ComPtr<ID3D12Device> dev;OK(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev)));
 D3D12_COMMAND_QUEUE_DESC qd={};ComPtr<ID3D12CommandQueue> queue;OK(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
 ComPtr<ID3D12CommandAllocator> alloc;OK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));
 ComPtr<ID3D12GraphicsCommandList> cl;OK(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&cl)));OK(cl->Close());
 D3D12_ROOT_PARAMETER rp={};rp.ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;rp.ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
 D3D12_ROOT_SIGNATURE_DESC rd={};rd.NumParameters=1;rd.pParameters=&rp;rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
 ComPtr<ID3DBlob> rb,err;OK(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&rb,&err));
 ComPtr<ID3D12RootSignature> root;OK(dev->CreateRootSignature(0,rb->GetBufferPointer(),rb->GetBufferSize(),IID_PPV_ARGS(&root)));
 char hlsl[8192]=
 "struct VSInput {float4 a0:ATTR0;};\n"
 "struct VSOutput {float4 pos:SV_Position;float4 t4:TEXCOORD4;};\n"
 "cbuffer VPConst:register(b0){float4 vp_c[512];float4 vp_posscale;\n    float4 vp_posoffset;\n};\n"
 "VSOutput main(VSInput input){float4 o[16];o[0]=float4(0,0,.5,1);o[6]=8;VSOutput Out;Out.pos=o[0];Out.t4=0;\n    return Out;\n}\n";
 assert(rsx_d3d12_point_quad_patch(hlsl,sizeof hlsl,rsx_d3d12_point_key(1,0x1001))==1);
 ComPtr<ID3DBlob> vs,ps;OK(D3DCompile(hlsl,strlen(hlsl),"point-test",nullptr,nullptr,"main","vs_5_0",0,0,&vs,&err));
 const char* ph="float4 main(float4 p:SV_Position,float4 t:TEXCOORD4):SV_Target{return float4(t.xy,1,1);}";
 OK(D3DCompile(ph,strlen(ph),"point-ps",nullptr,nullptr,"main","ps_5_0",0,0,&ps,&err));
 D3D12_INPUT_ELEMENT_DESC il={"ATTR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
 D3D12_GRAPHICS_PIPELINE_STATE_DESC pd={};pd.InputLayout={&il,1};pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
 pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
 auto &blend=pd.BlendState.RenderTarget[0];blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;blend.RenderTargetWriteMask=15;
 pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;pd.DepthStencilState.FrontFace.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;pd.DepthStencilState.FrontFace.StencilFailOp=pd.DepthStencilState.FrontFace.StencilDepthFailOp=pd.DepthStencilState.FrontFace.StencilPassOp=D3D12_STENCIL_OP_KEEP;pd.DepthStencilState.BackFace=pd.DepthStencilState.FrontFace;
 pd.SampleMask=UINT_MAX;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pd.SampleDesc.Count=1;
 ComPtr<ID3D12PipelineState> pso;OK(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso)));
 D3D12_HEAP_PROPERTIES hp={};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
 D3D12_RESOURCE_DESC td={};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=8;td.Height=8;td.DepthOrArraySize=1;td.MipLevels=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
 ComPtr<ID3D12Resource> rt;OK(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&rt)));
 D3D12_DESCRIPTOR_HEAP_DESC hd={};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;ComPtr<ID3D12DescriptorHeap> heap;OK(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));auto rtv=heap->GetCPUDescriptorHandleForHeapStart();dev->CreateRenderTargetView(rt.Get(),nullptr,rtv);
 D3D12_RESOURCE_DESC bd={};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=65536;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 hp.Type=D3D12_HEAP_TYPE_UPLOAD;ComPtr<ID3D12Resource> cb;OK(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&cb)));
 float* constants=nullptr;OK(cb->Map(0,nullptr,(void**)&constants));memset(constants,0,65536);constants[514*4]=8;constants[514*4+2]=64;constants[515*4]=constants[515*4+1]=1.f/8;
 hp.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> read;OK(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&read)));
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot;UINT rows;UINT64 rowbytes,total;dev->GetCopyableFootprints(&td,0,1,0,&foot,&rows,&rowbytes,&total);
 ComPtr<ID3D12Fence> fence;OK(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);assert(event);
 std::vector<unsigned char> reference;UINT64 serial=0;
 for(unsigned base:{0u,2u,5u,11u,18u,64031u,102744u}){
  constants[515*4+2]=(float)base; // catches the old erroneous subtraction
  OK(alloc->Reset());OK(cl->Reset(alloc.Get(),pso.Get()));cl->SetGraphicsRootSignature(root.Get());cl->SetGraphicsRootConstantBufferView(0,cb->GetGPUVirtualAddress());
  D3D12_VIEWPORT vp={0,0,8,8,0,1};D3D12_RECT sc={0,0,8,8};cl->RSSetViewports(1,&vp);cl->RSSetScissorRects(1,&sc);cl->OMSetRenderTargets(1,&rtv,FALSE,nullptr);float clear[4]={};cl->ClearRenderTargetView(rtv,clear,0,nullptr);cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cl->DrawInstanced(6,1,base,0);
  D3D12_RESOURCE_BARRIER barrier={};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={rt.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};cl->ResourceBarrier(1,&barrier);
  D3D12_TEXTURE_COPY_LOCATION dst={};dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=foot;D3D12_TEXTURE_COPY_LOCATION src={};src.pResource=rt.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;cl->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;cl->ResourceBarrier(1,&barrier);OK(cl->Close());ID3D12CommandList* lists[]={cl.Get()};queue->ExecuteCommandLists(1,lists);OK(queue->Signal(fence.Get(),++serial));OK(fence->SetEventOnCompletion(serial,event));assert(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);
  unsigned char* data=nullptr;D3D12_RANGE range={0,(SIZE_T)total};OK(read->Map(0,&range,(void**)&data));std::vector<unsigned char> pixels;for(unsigned y=0;y<8;y++)pixels.insert(pixels.end(),data+y*foot.Footprint.RowPitch,data+y*foot.Footprint.RowPitch+32);D3D12_RANGE empty={};read->Unmap(0,&empty);
  if(reference.empty()){reference=pixels;assert(reference[2]==255);}else if(pixels!=reference){fprintf(stderr,"FAIL: point quad changed with StartVertexLocation=%u\n",base);return 1;}
 }
 CloseHandle(event);cb->Unmap(0,nullptr);puts("PASS D3D12 WARP rendered pixels invariant for 7 nonzero/zero draw offsets");return 0;
}

// Headless hardware-contract regression for copying a currently bound D24S8
// attachment into a separate sampled colour snapshot and restoring the DSV.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "depth_shader_fixture.h" // exact production dsconv HLSL, extracted by runner
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
#define OK(x) do {HRESULT hr=(x);if(FAILED(hr)){fprintf(stderr,"HRESULT %08lX line %d\n",hr,__LINE__);return 1;}}while(0)
int main(){
 ComPtr<IDXGIFactory4> factory;OK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
 ComPtr<IDXGIAdapter> adapter;OK(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
 ComPtr<ID3D12Device> dev;OK(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev)));
 D3D12_COMMAND_QUEUE_DESC qd={};ComPtr<ID3D12CommandQueue> queue;OK(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
 ComPtr<ID3D12CommandAllocator> alloc;OK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));
 ComPtr<ID3D12GraphicsCommandList> cl;OK(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&cl)));OK(cl->Close());
 D3D12_DESCRIPTOR_RANGE range={};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=2;
 D3D12_ROOT_PARAMETER rp={};rp.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp.DescriptorTable={1,&range};rp.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
 D3D12_ROOT_SIGNATURE_DESC rd={};rd.NumParameters=1;rd.pParameters=&rp;
 ComPtr<ID3DBlob> sig,err,vs,ps;OK(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err));
 ComPtr<ID3D12RootSignature> root;OK(dev->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root)));
 OK(D3DCompile(depth_vs,strlen(depth_vs),"production-depth-vs",nullptr,nullptr,"main","vs_5_0",0,0,&vs,&err));
 OK(D3DCompile(depth_ps,strlen(depth_ps),"production-depth-ps",nullptr,nullptr,"main","ps_5_0",0,0,&ps,&err));
 D3D12_GRAPHICS_PIPELINE_STATE_DESC pd={};pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
 pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
 pd.BlendState.RenderTarget[0].RenderTargetWriteMask=15;pd.SampleMask=UINT_MAX;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pd.SampleDesc.Count=1;
 ComPtr<ID3D12PipelineState> pso;OK(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso)));
 D3D12_HEAP_PROPERTIES hp={};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
 D3D12_RESOURCE_DESC td={};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=8;td.Height=8;td.DepthOrArraySize=1;td.MipLevels=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
 ComPtr<ID3D12Resource> rt;OK(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&rt)));
 D3D12_RESOURCE_DESC zd=td;zd.Format=DXGI_FORMAT_R24G8_TYPELESS;zd.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
 ComPtr<ID3D12Resource> z;OK(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&zd,D3D12_RESOURCE_STATE_DEPTH_WRITE,nullptr,IID_PPV_ARGS(&z)));
 D3D12_DESCRIPTOR_HEAP_DESC hd={};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;ComPtr<ID3D12DescriptorHeap> rh;OK(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rh)));auto rtv=rh->GetCPUDescriptorHandleForHeapStart();dev->CreateRenderTargetView(rt.Get(),nullptr,rtv);
 hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;ComPtr<ID3D12DescriptorHeap> dh;OK(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dh)));auto dsv=dh->GetCPUDescriptorHandleForHeapStart();D3D12_DEPTH_STENCIL_VIEW_DESC dv={};dv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;dev->CreateDepthStencilView(z.Get(),&dv,dsv);
 hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;ComPtr<ID3D12DescriptorHeap> sh;OK(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&sh)));
 D3D12_SHADER_RESOURCE_VIEW_DESC sv={};sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;sv.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;auto handle=sh->GetCPUDescriptorHandleForHeapStart();dev->CreateShaderResourceView(z.Get(),&sv,handle);handle.ptr+=dev->GetDescriptorHandleIncrementSize(hd.Type);sv.Format=DXGI_FORMAT_X24_TYPELESS_G8_UINT;sv.Texture2D.PlaneSlice=1;dev->CreateShaderResourceView(z.Get(),&sv,handle);
 D3D12_RESOURCE_DESC bd={};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=65536;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 hp.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> read;OK(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&read)));
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot;UINT rows;UINT64 rowbytes,total;dev->GetCopyableFootprints(&td,0,1,0,&foot,&rows,&rowbytes,&total);
 ComPtr<ID3D12Fence> fence;OK(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);assert(event);
 auto barrier=[&](ID3D12Resource*r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER t={};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;t.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};cl->ResourceBarrier(1,&t);};
 const float depths[]={.125f,.875f,.5f,0.f,1.f};
 for(unsigned n=0;n<5;n++){
  OK(alloc->Reset());OK(cl->Reset(alloc.Get(),pso.Get()));
  cl->OMSetRenderTargets(1,&rtv,FALSE,&dsv);cl->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,depths[n],(UINT8)(0x31+n),0,nullptr);
  // The source is still the currently bound DSV, exactly the formerly skipped case.
  barrier(z.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  cl->OMSetRenderTargets(1,&rtv,FALSE,nullptr);cl->SetGraphicsRootSignature(root.Get());cl->SetPipelineState(pso.Get());ID3D12DescriptorHeap* heaps[]={sh.Get()};cl->SetDescriptorHeaps(1,heaps);cl->SetGraphicsRootDescriptorTable(0,sh->GetGPUDescriptorHandleForHeapStart());
  D3D12_VIEWPORT vp={0,0,8,8,0,1};D3D12_RECT sc={0,0,8,8};cl->RSSetViewports(1,&vp);cl->RSSetScissorRects(1,&sc);cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cl->DrawInstanced(3,1,0,0);
  barrier(z.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE);cl->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
  barrier(rt.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst={};dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=foot;D3D12_TEXTURE_COPY_LOCATION src={};src.pResource=rt.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;cl->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  barrier(rt.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);OK(cl->Close());ID3D12CommandList* lists[]={cl.Get()};queue->ExecuteCommandLists(1,lists);OK(queue->Signal(fence.Get(),n+1));OK(fence->SetEventOnCompletion(n+1,event));assert(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);
  unsigned char* bytes=nullptr;OK(read->Map(0,nullptr,(void**)&bytes));unsigned expected=(unsigned)std::round(depths[n]*16777215.f);
  for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){auto p=bytes+y*foot.Footprint.RowPitch+x*4;unsigned packed=((unsigned)p[3]<<16)|((unsigned)p[0]<<8)|p[1];assert(std::abs((int)packed-(int)expected)<=1);assert(p[2]==0x31+n);}
  D3D12_RANGE empty={};read->Unmap(0,&empty);
 }
 CloseHandle(event);puts("PASS D3D12 WARP current-DSV snapshots refresh across 5 depth/stencil generations (320 pixels)");return 0;
}

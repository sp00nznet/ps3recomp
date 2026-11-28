/* Runs the production render_frame dependency loop extracted by the runner.
 * Command recording is observed; the separate WARP test executes the shaders. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL depth dependency line %d\n",__LINE__); return 1; } } while(0)
typedef unsigned u32;
typedef void ID3D12DescriptorHeap;
typedef struct CommandList CommandList;
typedef struct { void (*SetGraphicsRootSignature)(CommandList*,void*); void (*SetDescriptorHeaps)(CommandList*,u32,void**); } Methods;
struct CommandList { Methods* lpVtbl; };
static int copies, roots, heaps, last_ds, last_rt;
static void* root_seen; static void* heap_seen[2];
static struct { struct {u32 gen,conv_gen;} off_ds[4]; CommandList* cmd_list; void *vp_root_sig,*srv_heap,*samp_heap; } s_d3d;
typedef struct {u32 fp_sampled_units;int tex_ds[16],tex_rt[16];} D3D12DrawRecord;
static void root(CommandList*c,void*r) {(void)c; roots++; root_seen=r;}
static void heap(CommandList*c,u32 n,void**h) {(void)c; heaps++; assert(n==2);heap_seen[0]=h[0];heap_seen[1]=h[1];}
static void ds_convert(int ds,int rt) { assert(ds>=0 && ds<4 && rt>=0); copies++;last_ds=ds;last_rt=rt;s_d3d.off_ds[ds].conv_gen=s_d3d.off_ds[ds].gen; }
static int record(const D3D12DrawRecord*dr,int cur_z,int samp_bound[16]) {
 int force_rebind=0;
 #include "depth_dependencies_fixture.h"
 return force_rebind;
}
int main(void) {
 Methods methods={root,heap}; CommandList cl={&methods}; int identity[3], samplers[16];
 s_d3d.cmd_list=&cl;s_d3d.vp_root_sig=&identity[0];s_d3d.srv_heap=&identity[1];s_d3d.samp_heap=&identity[2];
 D3D12DrawRecord dr;memset(&dr,0,sizeof dr);for(int i=0;i<16;i++){dr.tex_ds[i]=-1;dr.tex_rt[i]=-1;samplers[i]=i;}
 dr.tex_ds[0]=1;dr.tex_rt[0]=2;dr.fp_sampled_units=1;s_d3d.off_ds[1].gen=1;
 CHECK(record(&dr,1,samplers)==1 && copies==1 && last_ds==1 && last_rt==2);
 CHECK(roots==1 && heaps==1 && root_seen==s_d3d.vp_root_sig && heap_seen[0]==s_d3d.srv_heap && heap_seen[1]==s_d3d.samp_heap);
 for(int i=0;i<16;i++)CHECK(samplers[i]==-1);
 CHECK(record(&dr,1,samplers)==0 && copies==1); /* unchanged generation */
 s_d3d.off_ds[1].gen++;dr.fp_sampled_units=0;
 CHECK(record(&dr,1,samplers)==0 && copies==1); /* enabled but unread */
 dr.fp_sampled_units=(1u<<15);dr.tex_ds[15]=1;dr.tex_rt[15]=2;
 CHECK(record(&dr,1,samplers)==1 && copies==2); /* highest sampled unit */
 s_d3d.off_ds[1].gen++;dr.fp_sampled_units|=1;
 CHECK(record(&dr,1,samplers)==1 && copies==3); /* two units alias one depth */
 s_d3d.off_ds[1].gen++;
 CHECK(record(&dr,3,samplers)==1 && copies==4); /* noncurrent source */
 dr.tex_ds[0]=dr.tex_ds[15]=-1;
 CHECK(record(&dr,1,samplers)==0 && copies==4);
 puts("PASS: actual depth dependency loop: current DSV, generations, mask, aliases, root/heaps and forced rebind");return 0;
}

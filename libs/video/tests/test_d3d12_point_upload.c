/* Point register decoding, six-vertex upload and per-draw viewport constants
 * through the actual legacy D3D12 backend. Synthetic guest vertices only;
 * no device, window or title data. GPU coverage is tested separately.
 * python tools/test_d3d12_point_upload.py */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../rsx_d3d12_backend.c"

/* Only local synthetic vertex offsets are resolved; all other guest services
 * fail if unexpectedly called. */
u8* vm_base; u32 ppu_vm_size, ppu_hle_inject_base;
static void no_guest(const char* what) { fprintf(stderr, "FAIL fixture reached guest-side %s\n", what); exit(1); }
CellGcmDisplayInfo* cellGcmGetDisplayBufferByFlipIndex(u32 index) { (void)index; no_guest("display buffers"); return NULL; }
u32 cellGcmGetCurrentDisplayBufferId(void) { no_guest("display buffer id"); return 0; }
unsigned cellGcm_flip_request_count(void) { no_guest("flip count"); return 0; }
int cellGcm_current_scanout(u32* off, u32* pitch, u32* w, u32* h) { (void)off; (void)pitch; (void)w; (void)h; no_guest("scanout"); return 0; }
u32 cellGcmResolveOffset(u32 offset) { (void)offset; no_guest("offset"); return 0; }
u32 cellGcmResolveLocated(int location, u32 offset) { (void)location; return offset; }
u32 cellGcmResolveIO(u32 offset) { (void)offset; no_guest("IO offset"); return 0; }
int cellGcmOffsetIsDisplay(u32 offset) { (void)offset; no_guest("display offset"); return 0; }
u32 rsx_find_vram_upload(u32 size) { (void)size; no_guest("vram upload"); return 0; }
void rsx_reset_upload_claims(void) { no_guest("upload claims"); }
u32 gcm_io_offset_to_ea(u32 offset) { (void)offset; no_guest("IO map"); return 0; }
void vm_write32(uint32_t a, uint32_t v) { (void)a; (void)v; no_guest("memory write"); }
void ps3_ms(const char* key) { (void)key; }
unsigned long long ps3_ms_now(void) { return 0; }


#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL point upload line %d\n",__LINE__); return 1; } } while(0)
int main(void) {
 rsx_state state;rsx_state_init(&state);
 CHECK(state.point_size==1.0f && state.point_sprite_control==0);
 float size=8.0f;u32 bits;memcpy(&bits,&size,4);
 rsx_process_method(&state,0x1EE0,bits);rsx_process_method(&state,0x1EE8,0x1001);
 CHECK(state.point_size==8 && state.point_sprite_control==0x1001);
 vm_base=calloc(1,4096);CHECK(vm_base);ppu_vm_size=4096;
 state.vertex_attribs[0].enabled=1;state.vertex_attribs[0].type=2;state.vertex_attribs[0].size=4;
 state.vertex_attribs[0].stride=16;state.vertex_attribs[0].offset=256;
 for(u32 j=0;j<8;j++)for(u32 k=0;k<4;k++){float f=(float)(j*4+k);u32 u;memcpy(&u,&f,4);u8*p=vm_base+256+j*16+k*4;p[0]=u>>24;p[1]=u>>16;p[2]=u>>8;p[3]=u;}
 s_d3d.vp_vb_mapped=calloc(24,VP_VERT_STRIDE);CHECK(s_d3d.vp_vb_mapped);s_d3d.vp_vb_offset=2*VP_VERT_STRIDE;
 CHECK(upload_points_vp(&state,3,2)==12);
 VPSlot* out=(VPSlot*)((u8*)s_d3d.vp_vb_mapped+2*VP_VERT_STRIDE);
 for(u32 j=0;j<2;j++)for(u32 k=0;k<6;k++)for(u32 lane=0;lane<4;lane++)CHECK(out[(j*6+k)*16].v[lane]==(float)((3+j)*4+lane));
 CHECK(s_d3d.vp_vb_offset==14*VP_VERT_STRIDE);
 s_d3d.vp_vb_offset=(MAX_VERTICES-5)*VP_VERT_STRIDE;u32 offset=s_d3d.vp_vb_offset;
 CHECK(upload_points_vp(&state,0,1)==0 && s_d3d.vp_vb_offset==offset);
 s_d3d.current_rsx_state=&state;s_d3d.vp_cb_mapped=calloc(1,VP_CB_STRIDE);s_d3d.width=128;s_d3d.height=64;
 D3D12DrawRecord dr={0};dr.vp_w=32;dr.vp_h=16;vp_record_cb(0,-1,&dr);
 float* row=(float*)((u8*)s_d3d.vp_cb_mapped+RSX_MAX_VERTEX_CONSTANTS*16);
 CHECK(row[8]==8 && row[9]==0 && row[10]==64 && row[12]==1.0f/32 && row[13]==1.0f/16);
 state.vertex_attrib_output_mask=0x20;vp_record_cb(0,-1,&dr);CHECK(row[9]==1);
 free(s_d3d.vp_cb_mapped);free(s_d3d.vp_vb_mapped);free(vm_base);
 puts("PASS: production point methods, nonzero guest/upload offsets, whole-packet capacity and viewport constants");return 0;
}

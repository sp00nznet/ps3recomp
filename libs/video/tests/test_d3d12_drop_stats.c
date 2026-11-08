/* The legacy D3D12 path's draw-drop and flip counters, with no device: draws
 * reach the backend before its pipeline is up, with a bad vertex count and
 * without vertex-program resources, and each is counted by reason.
 * D3D12_DROP_STATS_EVERY=<n> prints the line on the nth flip; unset, nothing
 * is printed. No window, GPU, guest memory or game data.
 * python3 tools/test_d3d12_drop_stats.py */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../rsx_d3d12_backend.c"

/* The fixture has no guest: none of these may be reached on its paths. */
u8* vm_base; u32 ppu_vm_size, ppu_hle_inject_base;
static void no_guest(const char* what) { fprintf(stderr, "FAIL fixture reached guest-side %s\n", what); exit(1); }
CellGcmDisplayInfo* cellGcmGetDisplayBufferByFlipIndex(u32 index) { (void)index; no_guest("display buffers"); return NULL; }
u32 cellGcmGetCurrentDisplayBufferId(void) { no_guest("display buffer id"); return 0; }
unsigned cellGcm_flip_request_count(void) { no_guest("flip count"); return 0; }
int cellGcm_current_scanout(u32* off, u32* pitch, u32* w, u32* h) { (void)off; (void)pitch; (void)w; (void)h; no_guest("scanout"); return 0; }
u32 cellGcmResolveOffset(u32 offset) { (void)offset; no_guest("offset"); return 0; }
u32 cellGcmResolveLocated(int location, u32 offset) { (void)location; (void)offset; no_guest("located offset"); return 0; }
u32 cellGcmResolveIO(u32 offset) { (void)offset; no_guest("IO offset"); return 0; }
int cellGcmOffsetIsDisplay(u32 offset) { (void)offset; no_guest("display offset"); return 0; }
u32 rsx_find_vram_upload(u32 size) { (void)size; no_guest("vram upload"); return 0; }
void rsx_reset_upload_claims(void) { no_guest("upload claims"); }
u32 gcm_io_offset_to_ea(u32 offset) { (void)offset; no_guest("IO map"); return 0; }
void vm_write32(uint32_t a, uint32_t v) { (void)a; (void)v; no_guest("memory write"); }
void ps3_ms(const char* key) { (void)key; }
unsigned long long ps3_ms_now(void) { return 0; }

static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr, "FAIL %s\n", name); } \
                            else fprintf(stderr, "ok   %s\n", name); } while (0)

int main(void)
{
    /* Before the pipeline exists: both entry points drop as not_ready. */
    d3d12_draw_arrays(NULL, RSX_PRIMITIVE_TRIANGLES, 0, 3);
    d3d12_draw_indexed(NULL, RSX_PRIMITIVE_TRIANGLES, 0, 3);
    CHECK(s_d3d12_stats.calls == 2, "both draw entry points are counted");
    CHECK(s_d3d12_stats.drop[D3D12_DROP_NOT_READY] == 2, "draws before the pipeline drop as not_ready");

    /* A pipeline but an empty or oversized draw: bad_count. */
    s_d3d.pipeline_ready = 1; s_d3d.vb_mapped = 1;
    d3d12_draw_arrays(NULL, RSX_PRIMITIVE_TRIANGLES, 0, 0);
    d3d12_draw_indexed(NULL, RSX_PRIMITIVE_TRIANGLES, 0, MAX_VERTICES + 1);
    CHECK(s_d3d12_stats.drop[D3D12_DROP_BAD_COUNT] == 2, "empty and oversized draws drop as bad_count");

    /* An indexed draw without vertex-program resources: no_vp_resources. */
    d3d12_draw_indexed(NULL, RSX_PRIMITIVE_TRIANGLES, 0, 3);
    CHECK(s_d3d12_stats.drop[D3D12_DROP_NO_VP_RESOURCES] == 1, "indexed draw without VP resources is counted");
    CHECK(s_d3d12_stats.calls == 5, "every call is counted once");
    s_d3d.pipeline_ready = 0; s_d3d.vb_mapped = 0;

    /* The report: the driver checks stdout for the line (or its absence). */
    s_d3d12_stats.flips = 1;
    d3d12_print_stats();
    fflush(stdout);
    fprintf(stderr, "%s (%d/%d)\n", failures ? "FAILED" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}

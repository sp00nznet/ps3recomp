/* Synthetic command-processor test; no game, window, driver or GPU. */
#include "../libs/video/rsx_commands.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned clears, arrays, indexed, targets;
uint32_t ppu_hle_inject_base;
void vm_write32(uint32_t address, uint32_t value) { (void)address; (void)value; }
void ps3_ms(const char* key) { (void)key; }
static void clear(void* user, u32 mask, u32 color, float depth, u8 stencil)
{ (void)user; (void)mask; (void)color; (void)depth; (void)stencil; ++clears; }
static void draw(void* user, u32 prim, u32 first, u32 count)
{ (void)user; (void)prim; (void)first; (void)count; ++arrays; }
static void index_draw(void* user, u32 prim, u32 first, u32 count)
{ (void)user; (void)prim; (void)first; (void)count; ++indexed; }
static void set_target(void* user, const rsx_state* state)
{ (void)user; assert(state->surface_dma_set_mask == 0x1f); ++targets; }
static const u32 dma_methods[5] = {
    NV4097_SET_CONTEXT_DMA_COLOR_A, NV4097_SET_CONTEXT_DMA_COLOR_B,
    NV4097_SET_CONTEXT_DMA_COLOR_C, NV4097_SET_CONTEXT_DMA_COLOR_D,
    NV4097_SET_CONTEXT_DMA_ZETA
};
int main(int argc, char** argv)
{
    assert(argc == 2);
    rsx_state state;
    rsx_state_init(&state);
    rsx_set_backend(NULL);
    if (!strcmp(argv[1], "dma-local")) {
        assert(state.surface_dma_set_mask == 0);
        for (unsigned i=0; i<5; ++i) {
            state.surface_dirty=0;
            assert(rsx_process_method(&state,dma_methods[i],CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER)==0);
            assert(state.surface_dirty==1);
            assert(state.surface_dma_set_mask==((1u<<(i+1))-1));
            if(i<4) assert(state.surface_color_dma[i]==CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
            else assert(state.surface_zeta_dma==CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
        }
        rsx_backend backend={0}; backend.set_render_target=set_target;
        backend.clear=clear; backend.draw_arrays=draw; backend.draw_indexed=index_draw;
        rsx_set_backend(&backend);
        assert(rsx_process_method(&state,NV4097_SET_BEGIN_END,5)==0);
        assert(targets==1 && state.surface_dirty==0);
        assert(rsx_process_method(&state,NV4097_CLEAR_SURFACE,0xf0)==0);
        assert(rsx_process_method(&state,NV4097_DRAW_ARRAYS,0)==0);
        assert(rsx_process_method(&state,NV4097_DRAW_INDEX_ARRAY,0)==0);
        assert(clears==1 && arrays==1 && indexed==1);
        puts("local DMA bindings and backend state consumption verified"); return 0;
    }
    if (!strncmp(argv[1],"dma-host-",9) || !strncmp(argv[1],"dma-invalid-",12)) {
        int host=!strncmp(argv[1],"dma-host-",9);
        unsigned slot=(unsigned)strtoul(argv[1]+(host?9:12),NULL,10);
        assert(slot<5);
        u32 handle=host?CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER:0xfeed0002u;
        assert(rsx_process_method(&state,dma_methods[slot],handle)==-1);
        assert(state.surface_dma_set_mask==(1u<<slot));
        assert((slot<4?state.surface_color_dma[slot]:state.surface_zeta_dma)==handle);
        assert(state.surface_dirty==1);
        /* Ignoring a rejected programming method must not allow backend use. */
        rsx_backend backend={0}; backend.set_render_target=set_target;
        backend.clear=clear; backend.draw_arrays=draw; backend.draw_indexed=index_draw;
        rsx_set_backend(&backend);
        assert(rsx_process_method(&state,NV4097_SET_BEGIN_END,5)==-1);
        assert(!state.in_begin_end && !state.begin_epoch);
        assert(rsx_process_method(&state,NV4097_CLEAR_SURFACE,0xf0)==-1);
        assert(rsx_process_method(&state,NV4097_DRAW_ARRAYS,0)==-1);
        assert(rsx_process_method(&state,NV4097_DRAW_INDEX_ARRAY,0)==-1);
        assert(!targets && !clears && !arrays && !indexed);
        /* Rebinding a valid local handle clears the unsupported state. */
        assert(rsx_process_method(&state,dma_methods[slot],CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER)==0);
        assert(rsx_process_method(&state,NV4097_CLEAR_SURFACE,0xf0)==0 && clears==1);
        return 0;
    }
    if (!strcmp(argv[1], "unknown")) return rsx_process_method(&state, 0x1ffcu, 0xdeadbeefu) == -1 ? 0 : 1;
    if (!strcmp(argv[1], "clear")) return rsx_process_method(&state, NV4097_CLEAR_SURFACE, 0xf0);
    if (!strcmp(argv[1], "arrays")) return rsx_process_method(&state, NV4097_DRAW_ARRAYS, 0);
    if (!strcmp(argv[1], "indexed")) return rsx_process_method(&state, NV4097_DRAW_INDEX_ARRAY, 0);
    assert(!strcmp(argv[1], "backend"));
    rsx_backend backend = {0};
    backend.clear = clear;
    backend.draw_arrays = draw;
    backend.draw_indexed = index_draw;
    rsx_set_backend(&backend);
    assert(rsx_process_method(&state, NV4097_SET_COLOR_CLEAR_VALUE, 0x12345678u) == 0);
    assert(state.color_clear_value == 0x12345678u);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 0xf0) == 0);
    assert(rsx_process_method(&state, NV4097_DRAW_ARRAYS, 0) == 0);
    assert(rsx_process_method(&state, NV4097_DRAW_INDEX_ARRAY, 0) == 0);
    assert(clears == 1 && arrays == 1 && indexed == 1);
    puts("synthetic backend callbacks verified");
    return 0;
}

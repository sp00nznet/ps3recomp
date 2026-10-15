/* Synthetic RSX command-processor coverage; no driver, game or GPU. */
#include "../libs/video/rsx_commands.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

uint32_t ppu_hle_inject_base;
void vm_write32(uint32_t address, uint32_t value) { (void)address; (void)value; }
void ps3_ms(const char* key) { (void)key; }

typedef struct clear_record {
    unsigned bindings, clears;
    char sequence[32];
    unsigned sequence_len;
    u32 target, format, mask, color;
    u32 sx, sy, sw, sh;
    float depth;
    u8 stencil;
} clear_record;

static void bind_target(void* userdata, const rsx_state* state)
{
    clear_record* rec = (clear_record*)userdata;
    assert(rec->sequence_len + 1 < sizeof rec->sequence);
    rec->sequence[rec->sequence_len++] = 'B';
    ++rec->bindings;
    rec->target = state->surface_color_offset[0];
    rec->format = state->surface_format;
    rec->sx = state->scissor_x; rec->sy = state->scissor_y;
    rec->sw = state->scissor_w; rec->sh = state->scissor_h;
}

static void clear_target(void* userdata, u32 mask, u32 color, float depth, u8 stencil)
{
    clear_record* rec = (clear_record*)userdata;
    assert(rec->sequence_len + 1 < sizeof rec->sequence);
    rec->sequence[rec->sequence_len++] = 'C';
    ++rec->clears;
    rec->mask = mask; rec->color = color;
    rec->depth = depth; rec->stencil = stencil;
}

static void write_method(rsx_state* state, u32 method, u32 data)
{ assert(rsx_process_method(state, method, data) == 0); }

static void nearly(float actual, float expected)
{ assert(fabsf(actual - expected) < 0.000001f); }

static void setup(rsx_state* state, rsx_backend* backend, clear_record* record)
{
    memset(record, 0, sizeof *record);
    memset(backend, 0, sizeof *backend);
    backend->userdata = record;
    backend->set_render_target = bind_target;
    backend->clear = clear_target;
    rsx_state_init(state);
    assert(state->depth_func == 0x0201);
    assert(state->stencil_mask == 0xff && state->stencil_write_mask == 0xff);
    rsx_set_backend(backend);
}

static void test_binding_and_scissor(void)
{
    rsx_state state; rsx_backend backend; clear_record rec;
    setup(&state, &backend, &rec);
    write_method(&state, NV4097_SET_SURFACE_COLOR_AOFFSET, 0x1000);
    write_method(&state, NV4097_SET_SURFACE_FORMAT, 4u | (2u << 5));
    write_method(&state, NV4097_SET_COLOR_CLEAR_VALUE, 0x12345678);
    write_method(&state, NV4097_SET_SCISSOR_HORIZONTAL, (32u << 16) | 3u);
    write_method(&state, NV4097_SET_SCISSOR_VERTICAL, (16u << 16) | 7u);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 0xf0) == 0);
    assert(!state.in_begin_end && rec.bindings == 1 && rec.clears == 1);
    assert(rec.sequence_len == 2 && rec.sequence[0] == 'B' && rec.sequence[1] == 'C');
    assert(rec.target == 0x1000 && rec.format == (4u | (2u << 5)));
    assert(rec.sx == 3 && rec.sy == 7 && rec.sw == 32 && rec.sh == 16);
    assert(rec.mask == 0xf0 && rec.color == 0x12345678);
    assert(state.surface_dirty == 0 && state.viewport_dirty && state.blend_dirty);

    /* A retarget without a draw must reach the binding callback first. */
    write_method(&state, NV4097_SET_SURFACE_COLOR_AOFFSET, 0x2000);
    write_method(&state, NV4097_SET_SCISSOR_HORIZONTAL, (9u << 16) | 5u);
    assert(state.surface_dirty);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 0x10) == 0);
    assert(rec.bindings == 2 && rec.clears == 2 && rec.target == 0x2000);
    assert(rec.sx == 5 && rec.sw == 9 && rec.sequence[2] == 'B' && rec.sequence[3] == 'C');

    /* Even with no dirty surface, clear must rebind current state. */
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 0x80) == 0);
    assert(rec.bindings == 3 && rec.clears == 3);
    assert(rec.sequence[4] == 'B' && rec.sequence[5] == 'C');
}

static void test_depth_formats(void)
{
    rsx_state state; rsx_backend backend; clear_record rec;
    setup(&state, &backend, &rec);
    write_method(&state, NV4097_SET_SURFACE_FORMAT, 4u | (1u << 5));
    const struct { u32 value; float depth; } z16[] = {
        {0xabc00000u, 0.0f}, {0x0000ffffu, 1.0f},
        {0x89ab8000u, 32768.0f / 65535.0f}
    };
    for (unsigned i = 0; i < sizeof z16 / sizeof z16[0]; ++i) {
        write_method(&state, NV4097_SET_ZSTENCIL_CLEAR_VALUE, z16[i].value);
        assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 1) == 0);
        nearly(rec.depth, z16[i].depth);
        assert(rec.stencil == (u8)z16[i].value);
    }
    write_method(&state, NV4097_SET_SURFACE_FORMAT, 4u | (2u << 5));
    const struct { u32 value; float depth; u8 stencil; } z24[] = {
        {0x0000005au, 0.0f, 0x5a}, {0xffffff7fu, 1.0f, 0x7f},
        {0x800000e1u, 8388608.0f / 16777215.0f, 0xe1}
    };
    for (unsigned i = 0; i < sizeof z24 / sizeof z24[0]; ++i) {
        write_method(&state, NV4097_SET_ZSTENCIL_CLEAR_VALUE, z24[i].value);
        assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 3) == 0);
        nearly(rec.depth, z24[i].depth);
        assert(rec.stencil == z24[i].stencil);
    }
    assert(rec.bindings == 6 && rec.clears == 6);
}

static void test_failures_and_noop(void)
{
    rsx_state state; rsx_backend backend; clear_record rec;
    setup(&state, &backend, &rec);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 0) == 0);
    assert(!rec.bindings && !rec.clears);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 4) == -1);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 8) == -1);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 0x100) == -1);
    assert(!rec.bindings && !rec.clears);
    /* Color clear does not require a programmed depth format. */
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 0xf0) == 0);
    assert(rec.bindings == 1 && rec.clears == 1);
    /* SET_STENCIL_FUNC_MASK is the read mask; SET_STENCIL_MASK is write mask. */
    write_method(&state, NV4097_SET_STENCIL_FUNC_MASK, 0x35);
    assert(state.stencil_mask == 0x35 && state.stencil_write_mask == 0xff);
    write_method(&state, NV4097_SET_STENCIL_MASK, 0);
    assert(state.stencil_write_mask == 0 && state.stencil_dirty);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 2) == 0);
    assert(rec.bindings == 1 && rec.clears == 1);
    write_method(&state, NV4097_SET_STENCIL_MASK, 0x7f);
    assert(state.stencil_write_mask == 0x7f && state.stencil_mask == 0x35);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 2) == -1);
    assert(rec.bindings == 1 && rec.clears == 1);
    write_method(&state, NV4097_SET_STENCIL_MASK, 0xaabbccffu);
    assert(state.stencil_write_mask == 0xffu);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 1) == -1);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 2) == -1);
    assert(rec.bindings == 1 && rec.clears == 1);
    write_method(&state, NV4097_SET_SURFACE_FORMAT, 4u | (1u << 5));
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 2) == -1);
    assert(rec.bindings == 1 && rec.clears == 1);
    backend.clear = NULL;
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 0xf0) == -1);
    assert(rec.bindings == 1 && rec.clears == 1);
    rsx_set_backend(NULL);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 0xf0) == -1);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 0) == 0);
    backend.clear = clear_target; backend.set_render_target = NULL;
    rsx_set_backend(&backend);
    assert(rsx_process_method(&state, NV4097_CLEAR_SURFACE, 0x10) == 0);
    assert(rec.bindings == 1 && rec.clears == 2 && state.surface_dirty);
}

int main(int argc, char** argv)
{
    if (argc == 2) {
        rsx_state state; rsx_backend backend; clear_record rec;
        setup(&state, &backend, &rec);
        if (!strcmp(argv[1], "strict-mask"))
            rsx_process_method(&state, NV4097_CLEAR_SURFACE, 4);
        else if (!strcmp(argv[1], "strict-format"))
            rsx_process_method(&state, NV4097_CLEAR_SURFACE, 1);
        else if (!strcmp(argv[1], "strict-z16-stencil")) {
            write_method(&state, NV4097_SET_SURFACE_FORMAT, 4u | (1u << 5));
            rsx_process_method(&state, NV4097_CLEAR_SURFACE, 2);
        } else if (!strcmp(argv[1], "strict-partial-mask")) {
            write_method(&state, NV4097_SET_SURFACE_FORMAT, 4u | (2u << 5));
            write_method(&state, NV4097_SET_STENCIL_MASK, 0x7f);
            rsx_process_method(&state, NV4097_CLEAR_SURFACE, 2);
        } else if (!strcmp(argv[1], "strict-backend")) {
            rsx_set_backend(NULL);
            rsx_process_method(&state, NV4097_CLEAR_SURFACE, 0xf0);
        } else return 2;
        return 99; /* PS3_STRICT_BOOT=1 must stop with exit 86. */
    }
    assert(argc == 1);
    test_binding_and_scissor();
    test_depth_formats();
    test_failures_and_noop();
    puts("synthetic clear command, binding order and depth decode verified");
    return 0;
}

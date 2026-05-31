/*
 * ps3recomp - RSX FIFO parser test
 *
 * Verifies rsx_process_command_buffer: big-endian command-word decode and
 * method dispatch into the registered backend. This is the core of the
 * cellGcm -> RSX bridge (cellGcmSys's gcm_consume_fifo() just hands a
 * [get,put) slice of guest memory to this function).
 *
 * No D3D12: a tiny recording backend captures the dispatched calls.
 *
 * Build:
 *   gcc -std=c11 -O2 -I../../../include ../rsx_commands.c test_rsx_fifo.c -o t.exe
 */

#include "rsx_commands.h"
#include <stdio.h>
#include <string.h>

/* --- recording backend ------------------------------------------------- */
static int      rec_clear_calls;
static u32      rec_clear_color;
static int      rec_blend_calls;
static int      rec_blend_enable;
static int      rec_draw_calls;
static u32      rec_draw_prim, rec_draw_first, rec_draw_count;

static void rb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{ (void)ud;(void)flags;(void)depth;(void)stencil; rec_clear_calls++; rec_clear_color = color; }
static void rb_set_blend(void* ud, const rsx_state* s)
{ (void)ud; rec_blend_calls++; rec_blend_enable = s->blend_enable; }
static void rb_draw_arrays(void* ud, u32 prim, u32 first, u32 count)
{ (void)ud; rec_draw_calls++; rec_draw_prim = prim; rec_draw_first = first; rec_draw_count = count; }

static rsx_backend g_rec = {0};

/* --- big-endian FIFO writer -------------------------------------------- */
static u8  g_fifo[256];
static u32 g_fifo_len;

static void put_be32(u32 v)
{
    g_fifo[g_fifo_len++] = (u8)(v >> 24);
    g_fifo[g_fifo_len++] = (u8)(v >> 16);
    g_fifo[g_fifo_len++] = (u8)(v >> 8);
    g_fifo[g_fifo_len++] = (u8)(v);
}

/* One increasing method + single data word (count = 1). */
static void emit_method(u32 method, u32 data)
{
    u32 header = (1u << 18) | (((method >> 2) & 0x7FF) << 2); /* type 0, count 1 */
    put_be32(header);
    put_be32(data);
}

#define CHECK(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); } \
    else { printf("[FAIL] %s\n", msg); failures++; } } while (0)

int main(void)
{
    int failures = 0;

    g_rec.clear        = rb_clear;
    g_rec.set_blend    = rb_set_blend;
    g_rec.draw_arrays  = rb_draw_arrays;
    rsx_set_backend(&g_rec);

    rsx_state st;
    rsx_state_init(&st);

    /* Build a small frame: set clear color, clear, enable blend, draw a tri. */
    emit_method(NV4097_SET_COLOR_CLEAR_VALUE, 0xAABBCCDD);
    emit_method(NV4097_CLEAR_SURFACE,         0xF);
    emit_method(NV4097_SET_BLEND_ENABLE,      1);
    emit_method(NV4097_SET_BEGIN_END,         RSX_PRIMITIVE_TRIANGLES);
    emit_method(NV4097_DRAW_ARRAYS,           (2u << 24) | 0u); /* count 3, first 0 */
    emit_method(NV4097_SET_BEGIN_END,         0);

    int n = rsx_process_command_buffer(&st, (const u32*)g_fifo, g_fifo_len);

    printf("methods processed: %d\n", n);
    CHECK(n == 6, "all 6 methods dispatched");
    CHECK(rec_clear_calls == 1 && rec_clear_color == 0xAABBCCDD,
          "clear dispatched with correct color (BE decode)");
    CHECK(rec_blend_calls == 1 && rec_blend_enable == 1,
          "set_blend flushed on BEGIN_END with enable=1");
    CHECK(rec_draw_calls == 1 && rec_draw_prim == RSX_PRIMITIVE_TRIANGLES &&
          rec_draw_first == 0 && rec_draw_count == 3,
          "draw_arrays dispatched (prim=5, first=0, count=3)");

    printf("\n===========================================\n");
    printf("Results: %s (%d failure(s))\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}

/*
 * ps3recomp - Null RSX Backend (Win32 window + color clear)
 *
 * Implements rsx_backend callbacks using a simple Win32 GDI window.
 * No GPU rendering — just clears to the RSX clear color and presents.
 * Used for debugging command buffer flow before a real backend exists.
 */

#include "rsx_null_backend.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    HWND     hwnd;
    HDC      hdc;
    u32      width;
    u32      height;

    /* Current clear color (ARGB -> COLORREF) */
    COLORREF clear_color;

    /* Frame counter */
    u64      frame_count;
    u64      last_fps_time;
    u32      fps;

    int      window_closed;
} NullBackendState;

static NullBackendState s_state;

/* ---------------------------------------------------------------------------
 * Win32 window procedure
 * -----------------------------------------------------------------------*/

static LRESULT CALLBACK null_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        s_state.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        /* Fill with current clear color */
        HBRUSH brush = CreateSolidBrush(s_state.clear_color);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);

        /* Draw debug overlay text */
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        char buf[128];
        snprintf(buf, sizeof(buf), "ps3recomp null backend | %u FPS | frame %llu",
                 s_state.fps, (unsigned long long)s_state.frame_count);
        TextOutA(hdc, 10, 10, buf, (int)strlen(buf));

        char buf2[64];
        snprintf(buf2, sizeof(buf2), "Clear: #%06X",
                 (unsigned)(s_state.clear_color & 0xFFFFFF));
        TextOutA(hdc, 10, 30, buf2, (int)strlen(buf2));

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            s_state.window_closed = 1;
            DestroyWindow(hwnd);
        }
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------------------
 * Backend callbacks
 * -----------------------------------------------------------------------*/

static int null_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    printf("[RSX null] init(%ux%u)\n", width, height);
    return 0;
}

static void null_shutdown(void* ud)
{
    (void)ud;
    printf("[RSX null] shutdown\n");
}

static void null_begin_frame(void* ud)
{
    (void)ud;
}

static void null_end_frame(void* ud)
{
    (void)ud;
    s_state.frame_count++;

    /* Compute FPS every second */
    ULONGLONG now = GetTickCount64();
    if (now - s_state.last_fps_time >= 1000) {
        /* Simple: just use frame_count delta. For first second, estimate. */
        s_state.fps = (u32)(s_state.frame_count -
                           (s_state.frame_count > 60 ? s_state.frame_count - 60 : 0));
        s_state.last_fps_time = now;
    }
}

static void null_present(void* ud, u32 buffer_id)
{
    (void)ud;
    (void)buffer_id;

    /* Trigger a repaint */
    if (s_state.hwnd)
        InvalidateRect(s_state.hwnd, NULL, FALSE);
}

static void null_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    (void)flags;
    (void)depth;
    (void)stencil;

    /* Convert RSX ARGB to Win32 COLORREF (BGR) */
    u8 r = (color >> 16) & 0xFF;
    u8 g = (color >> 8) & 0xFF;
    u8 b = color & 0xFF;
    s_state.clear_color = RGB(r, g, b);
}

static void null_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    printf("[RSX null] set_render_target(format=0x%X, %ux%u)\n",
           state->surface_format, state->surface_clip_w, state->surface_clip_h);
}

static void null_set_viewport(void* ud, const rsx_state* state)
{
    (void)ud;
    printf("[RSX null] set_viewport(%u,%u %ux%u)\n",
           state->viewport_x, state->viewport_y,
           state->viewport_w, state->viewport_h);
}

static void null_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    static int s_draw_log_count = 0;
    if (s_draw_log_count < 20) {
        printf("[RSX null] draw_arrays(prim=%u, first=%u, count=%u)\n",
               primitive, first, count);
        s_draw_log_count++;
    }
}

static void null_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    (void)ud;
    static int s_draw_log_count = 0;
    if (s_draw_log_count < 20) {
        printf("[RSX null] draw_indexed(prim=%u, offset=%u, count=%u)\n",
               primitive, offset, count);
        s_draw_log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_null_backend = {
    .userdata          = &s_state,
    .init              = null_init,
    .shutdown          = null_shutdown,
    .begin_frame       = null_begin_frame,
    .end_frame         = null_end_frame,
    .present           = null_present,
    .set_render_target = null_set_render_target,
    .set_viewport      = null_set_viewport,
    .set_blend         = NULL,
    .set_depth_stencil = NULL,
    .clear             = null_clear,
    .draw_arrays       = null_draw_arrays,
    .draw_indexed      = null_draw_indexed,
    .bind_texture      = NULL,
};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rsx_null_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.width = width;
    s_state.height = height;
    s_state.clear_color = RGB(0, 0, 64); /* dark blue default */

    /* Register window class */
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = null_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "ps3recomp_null";
    RegisterClassExA(&wc);

    /* Compute window size from client area */
    RECT wr = {0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    s_state.hwnd = CreateWindowExA(
        0, "ps3recomp_null",
        title ? title : "ps3recomp",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);

    if (!s_state.hwnd) {
        printf("[RSX null] ERROR: CreateWindow failed (%lu)\n", GetLastError());
        return -1;
    }

    s_state.hdc = GetDC(s_state.hwnd);
    s_state.last_fps_time = GetTickCount64();

    /* Register as the active RSX backend */
    rsx_set_backend(&s_null_backend);

    printf("[RSX null] Window created: %ux%u\n", width, height);
    return 0;
}

void rsx_null_backend_shutdown(void)
{
    if (s_state.hwnd) {
        ReleaseDC(s_state.hwnd, s_state.hdc);
        DestroyWindow(s_state.hwnd);
        s_state.hwnd = NULL;
    }
    rsx_set_backend(NULL);
    printf("[RSX null] Backend shut down after %llu frames\n",
           (unsigned long long)s_state.frame_count);
}

int rsx_null_backend_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return -1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return s_state.window_closed ? -1 : 0;
}

#else /* !_WIN32 */

/* ---------------------------------------------------------------------------
 * Headless software backend
 *
 * No window and no GPU: the RSX target is a plain u32 framebuffer in host
 * memory, and triangles are filled on the CPU. That is enough to answer the
 * one question this backend exists to answer -- did the guest's command
 * stream actually reach a backend and produce the pixels it asked for -- on
 * any platform, including a CI runner with no display and no GPU driver.
 *
 * Linux has no real backend yet (Metal is Apple-only, D3D12 Windows-only), so
 * without this ps3recomp_host cannot be built or run there at all and the
 * whole cellGcm -> RSX -> backend bridge goes unexercised on the platform.
 *
 * ponytail: flat-shaded triangles from the first vertex's colour, no depth
 * buffer, no blending, no textures, no clipping beyond the framebuffer
 * bounds, and the viewport is the whole target. It is a correctness probe,
 * not a renderer. Anything more belongs in a real backend.
 * -----------------------------------------------------------------------*/

#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include <stdlib.h>

typedef struct {
    u32* fb;                /* width * height, 0x00RRGGBB          */
    u32  width, height;
    u32  clear_argb;        /* last NV4097_SET_COLOR_CLEAR_VALUE   */
    u32  last_present;      /* centre pixel of the last presented frame */
    int  presented;
    const rsx_state* state; /* live state, for the draw path       */

    /* One decoded texture unit. The point is not to be a sampler but to
     * prove the shared RSX texture path end to end -- layout, deswizzle,
     * channel order -- on a platform with no GPU at all. */
    u8*  tex;               /* decoded RGBA rows, tex_pitch * tex_h  */
    u32  tex_w, tex_h, tex_pitch;
    int  tex_ready;
} NullSoftState;

static NullSoftState s_soft;

static void nullsw_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud; (void)depth; (void)stencil;
    s_soft.clear_argb = color;
    if (!s_soft.fb || !(flags & 0xF0u)) return;   /* colour bits only */
    u32 rgb = color & 0x00FFFFFFu;
    for (u32 i = 0; i < s_soft.width * s_soft.height; i++) s_soft.fb[i] = rgb;
}

static void nullsw_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud; s_soft.state = state;
}

static void nullsw_set_vertex_attribs(void* ud, const rsx_state* state)
{
    (void)ud; s_soft.state = state;
}

/* NDC -> framebuffer pixels. The guest's clip-space position is divided by w
 * (1.0 for the identity-transform case) and mapped over the whole target;
 * see the ponytail note above about the viewport. */
static void ndc_to_px(const float p[4], float* sx, float* sy)
{
    float w = (p[3] != 0.0f) ? p[3] : 1.0f;
    *sx = ( p[0] / w * 0.5f + 0.5f) * (float)s_soft.width;
    *sy = (-p[1] / w * 0.5f + 0.5f) * (float)s_soft.height;
}

static u32 colour_of(const float c[4])
{
    float r = c[0] < 0 ? 0 : (c[0] > 1 ? 1 : c[0]);
    float g = c[1] < 0 ? 0 : (c[1] > 1 ? 1 : c[1]);
    float b = c[2] < 0 ? 0 : (c[2] > 1 ? 1 : c[2]);
    return ((u32)(r * 255.0f + 0.5f) << 16) |
           ((u32)(g * 255.0f + 0.5f) <<  8) |
            (u32)(b * 255.0f + 0.5f);
}

/* Decode the bound guest texture once, through the same rsx_texture_layout /
 * rsx_texture_decode the D3D12 backend uses. Only unit 0 is kept: this exists
 * to check that path produces the right pixels, not to be a texture cache. */
static void nullsw_bind_texture(void* ud, u32 unit, const rsx_texture_state* t)
{
    (void)ud;
    if (unit != 0 || !t) return;

    /* CONTROL0 bit 31 is the unit enable. A disabled unit unbinds. */
    if (!(t->control0 & 0x80000000u) || !t->offset) { s_soft.tex_ready = 0; return; }

    u32 w = (t->image_rect >> 16) & 0xFFFFu;
    u32 h =  t->image_rect        & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) { s_soft.tex_ready = 0; return; }

    /* Bit 31 of the offset selects MAIN vs LOCAL, same as a vertex array. */
    u32 ea = (t->offset & 0x80000000u)
                 ? cellGcmResolveLocated(0, t->offset & 0x7FFFFFFFu)
                 : cellGcmResolveLocated(1, t->offset & 0x7FFFFFFFu);
    if (!vm_base) { s_soft.tex_ready = 0; return; }

    rsx_tex_layout tl;
    rsx_texture_layout(t->format, w, h, &tl);
    if (tl.compressed) { s_soft.tex_ready = 0; return; }   /* no BC decode here */

    u32 pitch = w * 4u;
    u8* buf = (u8*)realloc(s_soft.tex, (size_t)pitch * h);
    if (!buf) { s_soft.tex_ready = 0; return; }
    s_soft.tex = buf;

    if (tl.fmt == RSX_TEXFMT_R8G8B8A8) {
        rsx_texture_decode(buf, pitch, vm_base + ea, w, h, &tl,
                           rsx_texture_argb_is_rgba());
    } else {
        /* Narrower formats decode to their own width; splay them to RGBA so
         * sampling below has one layout to read. */
        u32 srcp = tl.row_bytes;
        u8* tmp = (u8*)malloc((size_t)srcp * h);
        if (!tmp) { s_soft.tex_ready = 0; return; }
        rsx_texture_decode(tmp, srcp, vm_base + ea, w, h, &tl, 0);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) {
                const u8* sp = tmp + (size_t)y * srcp + (size_t)x * tl.bytes_per_texel;
                u8* dp = buf + (size_t)y * pitch + (size_t)x * 4u;
                dp[0] = sp[0];
                dp[1] = tl.bytes_per_texel > 1 ? sp[1] : sp[0];
                dp[2] = tl.bytes_per_texel > 2 ? sp[2] : sp[0];
                dp[3] = 255;
            }
        free(tmp);
    }

    s_soft.tex_w = w; s_soft.tex_h = h; s_soft.tex_pitch = pitch;
    s_soft.tex_ready = 1;
}

/* Nearest-neighbour point sample, wrapping. No filtering, no mip levels, no
 * addressing modes: the guest UVs in the harness land inside [0,1). */
static u32 sample_tex(float u, float v)
{
    if (!s_soft.tex_ready) return 0xFF00FFu;         /* magenta = not bound */
    long sx = (long)(u * (float)s_soft.tex_w);
    long sy = (long)(v * (float)s_soft.tex_h);
    sx %= (long)s_soft.tex_w; if (sx < 0) sx += (long)s_soft.tex_w;
    sy %= (long)s_soft.tex_h; if (sy < 0) sy += (long)s_soft.tex_h;
    const u8* px = s_soft.tex + (size_t)sy * s_soft.tex_pitch + (size_t)sx * 4u;
    return ((u32)px[0] << 16) | ((u32)px[1] << 8) | (u32)px[2];
}

/* Bounding-box scan with the three edge functions; fills when a pixel centre
 * is on the same side of all three edges, so winding does not matter. */
static void fill_triangle(const float a[4], const float b[4], const float c[4],
                          u32 rgb, const float uv[3][4], int textured)
{
    float ax, ay, bx, by, cx, cy;
    ndc_to_px(a, &ax, &ay); ndc_to_px(b, &bx, &by); ndc_to_px(c, &cx, &cy);

    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0.0f) return;                       /* degenerate */

    /* Bounding box. Plain casts truncate toward zero rather than flooring,
     * which only differs for negative coordinates -- and those clamp to 0 on
     * the next line anyway. +1 on the max side covers the truncation. Keeps
     * this file off libm. */
    float lox = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    float hix = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    float loy = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    float hiy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);

    long x0 = (long)lox,       y0 = (long)loy;
    long x1 = (long)hix + 1,   y1 = (long)hiy + 1;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > (long)s_soft.width)  x1 = (long)s_soft.width;
    if (y1 > (long)s_soft.height) y1 = (long)s_soft.height;

    for (long y = y0; y < y1; y++) {
        for (long x = x0; x < x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
            float w1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
            float w2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
            int neg = (w0 < 0) || (w1 < 0) || (w2 < 0);
            int pos = (w0 > 0) || (w1 > 0) || (w2 > 0);
            if (neg && pos) continue;               /* outside */

            u32 out = rgb;
            if (textured) {
                /* Barycentric weights fall out of the same edge functions, so
                 * interpolating costs nothing extra. Screen-space rather than
                 * perspective-correct: the harness draws in the z=0 plane,
                 * where the two agree. */
                float inv = 1.0f / area;
                float la = w1 * inv, lb = w2 * inv, lc = w0 * inv;
                float u = la * uv[0][0] + lb * uv[1][0] + lc * uv[2][0];
                float v = la * uv[0][1] + lb * uv[1][1] + lc * uv[2][1];
                out = sample_tex(u, v);
            }
            s_soft.fb[(u32)y * s_soft.width + (u32)x] = out;
        }
    }
}

/* Map sequence position -> guest vertex index, expanding the primitives the
 * hardware has and a triangle list does not. Returns the triangle count and
 * writes 3 indices per triangle through `emit`. */
static void draw_prim(u32 prim, u32 first, u32 count)
{
    const rsx_state* st = s_soft.state;
    if (!st || !s_soft.fb || count < 3) return;

    /* Position is attribute 0 and diffuse colour attribute 3 -- the same slots
     * the D3D12 and Metal fallback paths assume. */
    /* Texture coordinates are attribute 8 (texcoord0), the slot the D3D12 and
     * Metal fetch paths also read. A draw is textured when a texture is bound
     * AND that attribute is actually supplied. */
    const int textured = s_soft.tex_ready && st->vertex_attribs[8].enabled;

    #define TRI(i0, i1, i2) do {                                        \
        float p0[4], p1[4], p2[4], col[4], uv[3][4];                    \
        rsx_fetch_attrib(st, 0, (i0), p0);                              \
        rsx_fetch_attrib(st, 0, (i1), p1);                              \
        rsx_fetch_attrib(st, 0, (i2), p2);                              \
        rsx_fetch_attrib(st, 3, (i0), col);                             \
        rsx_fetch_attrib(st, 8, (i0), uv[0]);                           \
        rsx_fetch_attrib(st, 8, (i1), uv[1]);                           \
        rsx_fetch_attrib(st, 8, (i2), uv[2]);                           \
        fill_triangle(p0, p1, p2, colour_of(col), uv, textured);        \
    } while (0)

    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i + 2 < count; i += 3)
            TRI(first + i, first + i + 1, first + i + 2);
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; i++)
            TRI(first + i, first + i + 1 + (i & 1), first + i + 2 - (i & 1));
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; i++)
            TRI(first, first + i, first + i + 1);
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            TRI(first + i, first + i + 1, first + i + 2);
            TRI(first + i, first + i + 2, first + i + 3);
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            TRI(first + i,     first + i + 1, first + i + 3);
            TRI(first + i,     first + i + 3, first + i + 2);
        }
        break;
    default:
        break;   /* points and lines contribute no coverage here */
    }
    #undef TRI
}

static void nullsw_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    draw_prim(primitive, first, count);
}

static void nullsw_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    /* ponytail: index buffers are not read; the probe's geometry is
     * non-indexed. A backend that needs them reads them for real. */
    (void)ud; (void)primitive; (void)offset; (void)count;
}

static void nullsw_present(void* ud, u32 buffer_id)
{
    (void)ud; (void)buffer_id;
    if (!s_soft.fb) return;
    s_soft.last_present =
        s_soft.fb[(s_soft.height / 2) * s_soft.width + s_soft.width / 2];
    s_soft.presented++;
}

static rsx_backend s_nullsw_backend = {
    .userdata          = &s_soft,
    .set_render_target = nullsw_set_render_target,
    .set_vertex_attribs= nullsw_set_vertex_attribs,
    .clear             = nullsw_clear,
    .bind_texture      = nullsw_bind_texture,
    .draw_arrays       = nullsw_draw_arrays,
    .draw_indexed      = nullsw_draw_indexed,
    .present           = nullsw_present,
};

int rsx_null_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_soft, 0, sizeof(s_soft));
    s_soft.width  = width  ? width  : 1280;
    s_soft.height = height ? height : 720;
    s_soft.fb = (u32*)calloc((size_t)s_soft.width * s_soft.height, sizeof(u32));
    if (!s_soft.fb) return -1;
    rsx_set_backend(&s_nullsw_backend);
    printf("[RSX null] headless software backend %ux%u (%s)\n",
           s_soft.width, s_soft.height, title ? title : "");
    return 0;
}

void rsx_null_backend_shutdown(void)
{
    rsx_set_backend(NULL);
    free(s_soft.fb);   s_soft.fb  = NULL;
    free(s_soft.tex);  s_soft.tex = NULL;
    s_soft.tex_ready = 0;
}

/* Headless has no event queue and no window to close. */
int rsx_null_backend_pump_messages(void) { return 0; }

void rsx_null_backend_present(void) { nullsw_present(NULL, 0); }

u32 rsx_null_backend_debug_color(void) { return s_soft.clear_argb; }

u32 rsx_null_backend_readback_center(void)
{
    /* 0 means "nothing presented yet", so give a presented black pixel a
     * non-zero alpha the way a real drawable readback would. */
    return s_soft.presented ? (0xFF000000u | s_soft.last_present) : 0u;
}

#endif /* _WIN32 */

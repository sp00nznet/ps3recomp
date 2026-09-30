/*
 * ps3recomp - Vulkan RSX Backend
 *
 * The Linux renderer the README calls "the obvious target". Same host-facing
 * entry points and test hooks as the Metal and headless null backends, so
 * runtime/host/host_posix.c drives it unchanged.
 *
 * Two draw paths, one registered at a time (the FIFO walker feeds both):
 *
 *   - the shared register-file draw engine (rsx_draw_engine.h), the path the
 *     Metal backend takes. The engine owns surfaces, render-to-texture, MRT,
 *     depth textures, vertex compaction and pipeline keys; this backend builds
 *     Vulkan pipelines from the guest's programs (HLSL -> SPIR-V through
 *     glslang, rsx_shader_spirv.h) and executes. Every ps3recomp_host scene
 *     passes on it. Default when guest programs are on.
 *   - the rsx_state vtable: the headless null backend's fixed-function
 *     contract (flat colour, depth test, texture unit 0, point sampling), so
 *     the two agree pixel for pixel. Default otherwise; with guest programs it
 *     also runs them, but has no surfaces (--rtt, --depthtex, --mrt, --mrt-a).
 *
 * Rendering is offscreen; a present reads the frame back (the host's pixel
 * checks), optionally dumps it, and optionally shows it in a window.
 *
 * Vulkan is loaded at run time (dlopen of libvulkan.so.1), not linked, so:
 *   - building needs only the Vulkan headers, not a target-arch libvulkan
 *     (which is what makes cross-compiling for aarch64 straightforward);
 *   - a machine with no Vulkan loader fails init() cleanly instead of failing
 *     to start at all.
 * Only Vulkan 1.0 core entry points are used, so it runs on 1.2-level drivers
 * such as NVIDIA's L4T driver for Tegra X1.
 *
 * Environment:
 *   PS3RECOMP_VK_DEVICE=<n>   pick physical device n (vkEnumeratePhysicalDevices
 *                             order) instead of the first non-CPU device.
 *   PS3RECOMP_VK_DUMP=<path>  write every presented frame to <path> as a
 *                             binary PPM (overwritten each present).
 *   PS3RECOMP_VK_WINDOW=1     also show each presented frame in an SDL2
 *                             window. Rendering and readback are unchanged, so
 *                             every test gives the same result with or without
 *                             it; if the window cannot be opened the run
 *                             continues headless and says why.
 *   PS3RECOMP_VK_FULLSCREEN=1 with a window: borderless full screen.
 *   PS3RECOMP_VK_HOLD=<sec>   with a window: keep the last frame on screen
 *                             this long before shutdown closes it.
 *   PS3RECOMP_VK_GUEST_PROGRAMS=1
 *                             run the guest's own vertex/fragment programs
 *                             (needs the translator: glslang at build time),
 *                             which also makes the draw engine the default.
 *   PS3RECOMP_RSX_ENGINE=dispatch|vtable
 *                             pick the path explicitly (the engine's own
 *                             switch, shared with the Metal backend).
 */

#ifndef PS3RECOMP_RSX_VULKAN_BACKEND_H
#define PS3RECOMP_RSX_VULKAN_BACKEND_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load Vulkan, create the device and the offscreen target, and register as
 * the RSX backend. `title` is only logged (no window at V0).
 * Returns 0 on success, -1 on any failure (reason printed to stderr). */
int  rsx_vulkan_backend_init(u32 width, u32 height, const char* title);

/* Unregister and destroy every Vulkan object, then unload the library. */
void rsx_vulkan_backend_shutdown(void);

/* Headless: always 0. Windowed: drains SDL's event queue; -1 once the
 * window has been closed or Esc pressed. */
int  rsx_vulkan_backend_pump_messages(void);

/* Read the frame back to host memory (and to PS3RECOMP_VK_DUMP if set). */
void rsx_vulkan_backend_present(void);

/* The clear colour most recently set through the command stream, ARGB8888. */
u32  rsx_vulkan_backend_debug_color(void);

/* Centre pixel of the last presented frame as 0xFFRRGGBB, or 0 if nothing
 * has been presented yet. */
u32  rsx_vulkan_backend_readback_center(void);

/* Draws of the last presented frame that ran the guest's own programs. */
u32  rsx_vulkan_backend_guest_draws(void);

/* 1 when draws run the guest's own programs: the HLSL -> SPIR-V translator
 * was built (glslang found) and PS3RECOMP_VK_GUEST_PROGRAMS=1 opts in. Then
 * the shared register-file draw engine (rsx_draw_engine.h) is the default
 * path, as it is Metal's; PS3RECOMP_RSX_ENGINE=vtable selects the older
 * rsx_state path instead. Opt-in while the path grows; needs no initialised
 * backend. */
int  rsx_vulkan_backend_guest_programs(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VULKAN_BACKEND_H */

/*
 * ps3recomp - Vulkan RSX Backend
 *
 * The Linux renderer the README calls "the obvious target". Same host-facing
 * entry points and test hooks as the Metal and headless null backends, so
 * runtime/host/host_posix.c drives it unchanged.
 *
 * Stage V1 (this file): offscreen colour + depth targets, NV4097 CLEAR_SURFACE
 * (colour and depth), the fallback draw path (the headless null backend's
 * contract: flat colour, depth test, texture unit 0, point sampling), and
 * present with a CPU readback of the frame. Every ps3recomp_host scene passes
 * on it. No window yet, no guest shaders yet -- those are the next stages.
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

/* No window at V0, so no event queue: always 0. */
int  rsx_vulkan_backend_pump_messages(void);

/* Read the frame back to host memory (and to PS3RECOMP_VK_DUMP if set). */
void rsx_vulkan_backend_present(void);

/* The clear colour most recently set through the command stream, ARGB8888. */
u32  rsx_vulkan_backend_debug_color(void);

/* Centre pixel of the last presented frame as 0xFFRRGGBB, or 0 if nothing
 * has been presented yet. */
u32  rsx_vulkan_backend_readback_center(void);

/* Draws of the last presented frame that ran the guest's own programs:
 * 0 at V0, there is no shader path yet. */
u32  rsx_vulkan_backend_guest_draws(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VULKAN_BACKEND_H */

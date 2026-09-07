/*
 * ps3recomp - RSX -> Metal backend (macOS / Apple Silicon)
 *
 * Mirrors the entry-point shape of rsx_d3d12_backend.h so a host can select a
 * backend at build time without any other change. Unlike the D3D12 backend this
 * file is NOT gated on a platform macro: the implementation lives in
 * rsx_metal_backend.m and is only compiled into the library on Apple targets
 * (see CMakeLists.txt), so there is nothing to stub out elsewhere.
 *
 * Headless mode: set PS3RECOMP_METAL_HEADLESS=1 to render to an offscreen
 * MTLTexture with no window or NSApplication. Everything below behaves
 * identically, which is what makes the backend testable in CI on a machine
 * with no display.
 */
#ifndef PS3RECOMP_RSX_METAL_BACKEND_H
#define PS3RECOMP_RSX_METAL_BACKEND_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the device, surface and command queue, and register the backend with
 * the RSX command processor. Returns 0 on success, -1 on failure. */
int  rsx_metal_backend_init(u32 width, u32 height, const char* title);

/* Unregister, then tear down the surface and device. Safe if init failed. */
void rsx_metal_backend_shutdown(void);

/* Pump the native event queue. Returns -1 when the window has been closed,
 * 0 otherwise. Always returns 0 in headless mode. */
int  rsx_metal_backend_pump_messages(void);

/* Present one frame: clear the drawable to the colour last received through the
 * RSX clear method, then hand it to the compositor. */
void rsx_metal_backend_present(void);

/* --- test hooks ---------------------------------------------------------- */

/* The clear colour most recently set via NV4097_SET_COLOR_CLEAR_VALUE, in the
 * RSX's native ARGB8888. Lets a host assert the guest command stream actually
 * reached the backend. */
u32  rsx_metal_backend_debug_color(void);

/* Read the centre pixel of the last presented frame as BGRA8888, or 0 if
 * nothing has been presented. Headless mode only -- returns 0 with a live
 * drawable, whose contents belong to the compositor once presented. */
u32  rsx_metal_backend_readback_center(void);

/* How many draws of the last presented frame ran the guest's own vertex and
 * fragment programs rather than the built-in shader. A host mode that loads
 * programs asserts this is non-zero, because the built-in path would draw
 * the same test triangle and a pixel check alone could not tell them apart. */
u32  rsx_metal_backend_guest_draws(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_METAL_BACKEND_H */

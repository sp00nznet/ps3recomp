/*
 * ps3recomp - Null RSX Backend (Win32 window + color clear)
 *
 * Minimal graphics backend that:
 * - Creates a Win32 window
 * - Clears to the RSX clear color on each frame
 * - Presents via GDI (no GPU acceleration)
 *
 * This is the first step toward rendering — it proves the RSX command
 * processor is receiving commands and the game loop is running. Replace
 * with D3D12 or Vulkan backend for actual rendering.
 */

#ifndef PS3RECOMP_RSX_NULL_BACKEND_H
#define PS3RECOMP_RSX_NULL_BACKEND_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create and register the null backend.
 * Opens a window of the specified size and starts accepting RSX commands.
 * Returns 0 on success. */
int rsx_null_backend_init(u32 width, u32 height, const char* title);

/* Shut down the null backend and close the window. */
void rsx_null_backend_shutdown(void);

/* Process Win32 messages. Call this from the game's main loop
 * or from a timer/idle callback. Returns 0 normally, -1 if the
 * window was closed. */
int rsx_null_backend_pump_messages(void);

#ifdef __cplusplus
}
#endif
#endif

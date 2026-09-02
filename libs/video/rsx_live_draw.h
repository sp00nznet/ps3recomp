/*
 * ps3recomp - Track B live NV4097 draw path (LAYER 2 -> live backend)
 *
 * The offline capture-replay harness (libs/video/tests/replay_main.c) proved a
 * full NV4097 -> D3D12 draw pipeline: the rsx_dispatch register-file model
 * feeds an NV40 VP/FP -> HLSL decompiler and a PSO/sampler/texture engine that
 * renders real geometry into a color surface. This module lifts that engine out
 * of the harness so the LIVE FIFO consumer (yakuza/import_overrides.cpp
 * yz_rsx_method) can drive it with the game's own method stream, rendering into
 * a D3D12 present surface.
 *
 * Data flow (live):
 *   game (t1) writes NV methods to the FIFO ring
 *     -> yz_rsx_method(method, arg)                         [existing hook]
 *        -> rsx_live_draw_method(method, arg)               [this module]
 *           -> rsx_dispatch_method(...)  (register file + BEGIN/END/DRAW/FLIP)
 *              -> sink callbacks -> D3D12 PSO + draw into the flip surface
 *   flip method (0xE944) -> rsx_live_draw_present(buffer_id) -> Present()
 *
 * The whole path is gated by the YZ_RSX_DRAW env flag (see docs/FLAGS.md): when
 * unset/"0" the live draw engine is inert and the runtime keeps its existing
 * (null / clear-color) present, so this is a pure add-on with a kill switch.
 *
 * Clean-room: method numbers/semantics from envytools rnndb + Mesa nv30 +
 * psdevwiki; RPCS3 consulted only as a read-only fact oracle. No GPL code.
 *
 * Windows/D3D12 only; a no-op stub compiles elsewhere.
 */

#ifndef PS3RECOMP_RSX_LIVE_DRAW_H
#define PS3RECOMP_RSX_LIVE_DRAW_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Guest-memory resolver: given an RSX location (0 = RSX local VRAM,
 * 1 = main/IO memory) and a byte offset, return a host pointer to at least
 * `min_bytes` of readable guest memory, or NULL if the region is invalid.
 * The runtime supplies this (it owns the vm_base map + the gcm local carve).
 * The replay harness supplies its own arena-backed version. */
typedef const u8* (*rsx_live_guest_ptr_fn)(void* user, u32 location, u32 offset,
                                           u32 min_bytes);

/* Whether the live draw path is enabled this run (reads YZ_RSX_DRAW once).
 * Cheap; safe to call from the hot method hook. */
int  rsx_live_draw_enabled(void);

/* Bring up the D3D12 device + swap chain bound to an existing HWND (the
 * runtime already opens the present window). Returns 0 on success. Safe to
 * call when disabled (returns 0 and stays inert). `hwnd` is an HWND cast to
 * void* to keep this header windows.h-free. */
int  rsx_live_draw_init(void* hwnd, u32 width, u32 height,
                        rsx_live_guest_ptr_fn guest_ptr, void* guest_user);

/* Seed the dispatcher's register file / transform program from a captured or
 * live initial context (optional; the live path can also just stream). */
void rsx_live_draw_seed_registers(const u32* regs, u32 count);
void rsx_live_draw_seed_transform_program(const u32* words, u32 count);
void rsx_live_draw_seed_transform_constants(const u32* words, u32 count);

/* Register a scanout buffer from sys_rsx_context_attribute(0x104). A flip's
 * argument indexes this table; it is not necessarily the current render
 * target when the frame is submitted. */
void rsx_live_draw_set_display_buffer(u32 buffer_id, u32 location, u32 offset,
                                      u32 pitch, u32 width, u32 height);

/* Feed one NV method write. No-op when disabled or uninitialized. */
void rsx_live_draw_method(u32 method, u32 arg);

/* Publish the FIFO bounds associated with the next flip method.  The live
 * consumer calls this only for driver-queue methods; ordinary method traffic
 * pays no extra synchronization cost. */
void rsx_live_draw_set_fifo_position(u32 get, u32 put);

/* Record an NV308A inline-to-memory write that overlaps a registered color
 * surface.  Guest memory changes are deliberately tracked separately from
 * actual D3D render-target mutations. */
void rsx_live_draw_note_inline_transfer(u32 dma, u32 offset, u32 value);

/* Block until the GPU finishes all queued draws (RSX SET_REFERENCE / sync
 * fence). Mirrors RPCS3 nv406e::set_reference's sync() so REF advances only
 * after the GPU has really caught up. */
void rsx_live_draw_flush(void);

/* Present the given display buffer id (called from the flip path). */
void rsx_live_draw_present(u32 buffer_id);

/* Movie playback: while on, the guest method stream is ignored so a host-decoded
 * movie can own the window without the guest's draws racing g.list. */
void rsx_live_draw_set_movie_mode(int on);

/* Present a host-decoded RGBA8 frame (w*h*4, row pitch w*4) straight to the
 * swap-chain backbuffer. Call from one thread with movie mode on. */
void rsx_live_draw_present_rgba(const uint8_t* rgba, u32 w, u32 h);

/* Count of REAL game frames the live-draw engine has presented (the game's own
 * 0xE944 flips carrying accumulated draws) -- distinct from the null backend's
 * per-vblank "flips" present count, which keeps ticking even when t1 has stalled
 * and stopped producing. Cheap; safe to poll from the title path. */
u32  rsx_live_draw_get_frames(void);

/* Profile-only access to the host thread that consumes the live FIFO and
 * presents game frames. Returns NULL outside the instrumented sampler build.
 * The opaque handle keeps windows.h out of this public header. */
void* rsx_live_draw_get_present_thread_handle(void);

/* Draws in the last completed frame (title-bar telemetry). */
u32  rsx_live_draw_get_last_draws(void);

/* Informational rolling presented FPS for the window title. The benchmark
 * recomputes its selected interval from the preserved raw QPC samples. */
double rsx_live_draw_get_present_fps(void);

/* End-only persistence hook used by the window-close path. Safe to call more
 * than once; it writes the fixed QPC ring at most once per renderer run. */
void rsx_live_draw_dump_present_samples(void);

/* Bounded live AUTH diagnostic. The null-backend window starts it from a
 * private WM_APP message immediately before New Game is confirmed. */
#define RSX_LIVE_DRAW_WM_A010_PROBE (0x8000u + 0x2A0u)
void rsx_live_draw_a010_probe_begin(void);

/* Supply/clear a010's reconstructed view-projection matrix. The live backend
 * uses it only when a draw's c108..c111 rows are NaNs, preserving finite
 * constant uploads and all rendering outside the scoped AUTH repair. */
void rsx_live_draw_set_a010_camera_matrix(const float* matrix16);
int  rsx_live_draw_a010_probe_active(void);
/* Becomes true after the first real orphanage character mesh has reached the
 * live draw sink. AUTH playback uses this to avoid outrunning asynchronous
 * model submission while the scene is still visually empty. */
int  rsx_live_draw_a010_world_ready(void);

/* Offline differential-test hook.  Dumps one tracked render surface without
 * changing its state/lifetime contract.  The production title never calls
 * this; capture replay uses it to compare the live backend against the
 * independent replay renderer at the same guest surface boundary. */
int  rsx_live_draw_debug_dump_surface(u32 location, u32 offset,
                                      const char* path);

/* Release all D3D12 resources. */
void rsx_live_draw_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_LIVE_DRAW_H */

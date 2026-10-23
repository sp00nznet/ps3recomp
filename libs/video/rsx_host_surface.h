/*
 * ps3recomp - one window across an executable swap (macOS)
 *
 * A title that replaces its own executable (sys_game_process_exitspawn:
 * Resistance goes from the EBOOT's menus to game.self) loses every AppKit
 * object with the old process image, so its window closes and a new one
 * opens. To keep one window, the window lives in a small launcher process
 * (tools/host_window) that never execs, and the game renders into IOSurfaces
 * the launcher owns. The game side is the Metal backend's host-surface mode
 * (rsx_metal_backend.m, PS3RECOMP_HOST_SURFACES).
 *
 *   launcher                                   game process
 *   bootstrap_check_in(name)                   bootstrap_look_up($PS3RECOMP_HOST_SURFACES)
 *        <-- ATTACH (width, height, pid, reply port) --
 *        -- SURFACES (control page, N IOSurface ports, size) -->
 *        <-- FRAME (header only; dropped when the queue is full) --
 *
 * Nothing in the game process needs a file descriptor or a port that an exec
 * would lose: the service name is in the environment, and the image that
 * comes up after an exec looks it up and attaches exactly as the first one
 * did. The launcher keeps the same surfaces and the same control page, so the
 * window keeps showing the last frame of the old image until the new one
 * publishes its first.
 *
 * The control page is a mach memory entry both processes map. Each surface
 * has one atomic state word:
 *
 *   FREE --game--> RENDERING --game, GPU done--> READY --launcher--> READING
 *     ^                |                           |                    |
 *     +----------------+ (frame failed)            +--game (reclaim)----+--launcher, blit done--> FREE
 *
 * A side only moves a surface out of a state it is allowed to leave, and
 * always with a compare-and-swap, so neither process ever writes a surface
 * the other is reading. The game needs no CPU copy and no wait on the
 * launcher: when every surface is busy it reclaims the oldest READY one
 * (that frame is dropped, a newer one is coming).
 *
 * Header-only and plain C so the launcher, the backend and the tests share the
 * one implementation of the protocol.
 */
#ifndef PS3RECOMP_RSX_HOST_SURFACE_H
#define PS3RECOMP_RSX_HOST_SURFACE_H

#include <mach/mach.h>
#include <stdatomic.h>
#include <stdint.h>

/* The launcher's bootstrap service name. Its presence is what puts the Metal
 * backend in host-surface mode. */
#define RSX_HOST_SURFACE_ENV     "PS3RECOMP_HOST_SURFACES"

#define RSX_HOST_MAGIC           0x50335753u     /* "P3WS" */
#define RSX_HOST_VERSION         1u
/* MTL_MAX_INFLIGHT (3) frames rendering, one being read, one READY: with five
 * the game always finds a surface it may take. */
#define RSX_HOST_SLOTS           5
#define RSX_HOST_MAX_DIM         8192u
#define RSX_HOST_CONTROL_BYTES   16384u          /* one arm64 page */

enum {
    RSX_HOST_FREE      = 0,
    RSX_HOST_RENDERING = 1,
    RSX_HOST_READY     = 2,
    RSX_HOST_READING   = 3,
};

enum {
    RSX_HOST_MSG_ATTACH   = 0x50330001,
    RSX_HOST_MSG_SURFACES = 0x50330002,
    RSX_HOST_MSG_FRAME    = 0x50330003,
};

typedef struct {
    uint32_t magic, version;
    uint32_t width, height;            /* the surfaces' size, BGRA8 */
    uint32_t generation;               /* bumped whenever the surfaces are replaced */
    int32_t  launcher_pid;
    _Atomic int32_t  producer_pid;     /* the image attached last */
    _Atomic uint32_t closed;           /* launcher: the window is closing */
    _Atomic uint64_t guest_frames;     /* rsx_metal_backend_note_guest_frame */
    _Atomic uint64_t next_seq;         /* publish order; survives the game's exec */
    _Atomic uint64_t published;        /* frames made READY */
    _Atomic uint64_t reclaimed;        /* READY frames the game took back unshown */
    _Atomic uint64_t skipped;          /* presents with no surface to render to */
    _Atomic uint32_t state[RSX_HOST_SLOTS];
    _Atomic uint64_t seq[RSX_HOST_SLOTS];
    _Atomic uint64_t publish_time[RSX_HOST_SLOTS];   /* mach_absolute_time() */
} rsx_host_control;

_Static_assert(sizeof(rsx_host_control) <= RSX_HOST_CONTROL_BYTES, "control page");

/* Messages. Both ends are built from this header, so the layouts agree. */
typedef struct {
    mach_msg_header_t header;          /* reply port: MAKE_SEND_ONCE */
    uint32_t width, height;            /* the size the game would like; 0 = keep */
    int32_t  pid;
} rsx_host_attach_msg;

typedef struct {
    mach_msg_header_t          header;
    mach_msg_body_t            body;
    mach_msg_port_descriptor_t control;                  /* memory entry */
    mach_msg_port_descriptor_t surface[RSX_HOST_SLOTS];  /* IOSurfaceCreateMachPort */
    uint32_t width, height, generation;
    int32_t  launcher_pid;
} rsx_host_surfaces_msg;

typedef struct {
    mach_msg_header_t header;
} rsx_host_frame_msg;

/* ---- game side ----------------------------------------------------------- */

/* A surface to render the next frame into: a FREE one, or failing that the
 * oldest READY one (its frame is dropped). -1 when every surface is
 * rendering or being read. */
static inline int rsx_host_begin_render(rsx_host_control* c)
{
    for (int i = 0; i < RSX_HOST_SLOTS; i++) {
        uint32_t s = RSX_HOST_FREE;
        if (atomic_compare_exchange_strong(&c->state[i], &s, RSX_HOST_RENDERING))
            return i;
    }
    for (int tries = 0; tries < 4; tries++) {
        int oldest = -1;
        uint64_t oldest_seq = UINT64_MAX;
        for (int i = 0; i < RSX_HOST_SLOTS; i++) {
            if (atomic_load(&c->state[i]) != RSX_HOST_READY) continue;
            const uint64_t q = atomic_load(&c->seq[i]);
            if (q < oldest_seq) { oldest_seq = q; oldest = i; }
        }
        if (oldest < 0) break;
        uint32_t s = RSX_HOST_READY;
        if (atomic_compare_exchange_strong(&c->state[oldest], &s, RSX_HOST_RENDERING)) {
            atomic_fetch_add(&c->reclaimed, 1);
            return oldest;
        }
    }
    atomic_fetch_add(&c->skipped, 1);
    return -1;
}

/* The GPU is done with `slot`: publish it (ok) or hand it back. */
static inline void rsx_host_end_render(rsx_host_control* c, int slot, int ok, uint64_t now)
{
    if (slot < 0 || slot >= RSX_HOST_SLOTS) return;
    if (ok) {
        atomic_store(&c->seq[slot], atomic_fetch_add(&c->next_seq, 1) + 1);
        atomic_store(&c->publish_time[slot], now);
        atomic_fetch_add(&c->published, 1);
    }
    uint32_t s = RSX_HOST_RENDERING;
    atomic_compare_exchange_strong(&c->state[slot], &s, ok ? RSX_HOST_READY : RSX_HOST_FREE);
}

/* ---- launcher side ------------------------------------------------------- */

/* The newest READY surface published after `after`, now READING; -1 if there
 * is none. Older READY surfaces are freed on the way: they will never be
 * shown. */
static inline int rsx_host_begin_read(rsx_host_control* c, uint64_t after, uint64_t* seq_out)
{
    for (int tries = 0; tries < 8; tries++) {
        int best = -1;
        uint64_t best_seq = after;
        for (int i = 0; i < RSX_HOST_SLOTS; i++) {
            if (atomic_load(&c->state[i]) != RSX_HOST_READY) continue;
            const uint64_t q = atomic_load(&c->seq[i]);
            if (q > best_seq) { best_seq = q; best = i; }
        }
        if (best < 0) return -1;
        uint32_t s = RSX_HOST_READY;
        if (!atomic_compare_exchange_strong(&c->state[best], &s, RSX_HOST_READING))
            continue;                              /* the game reclaimed it; look again */
        /* Read the sequence again now that the game cannot touch the slot:
         * it may have been reclaimed and republished between the scan and the
         * swap, which only ever makes it newer. */
        const uint64_t q = atomic_load(&c->seq[best]);
        if (q <= after) {
            s = RSX_HOST_READING;
            atomic_compare_exchange_strong(&c->state[best], &s, RSX_HOST_FREE);
            continue;
        }
        for (int i = 0; i < RSX_HOST_SLOTS; i++) {
            if (i == best || atomic_load(&c->state[i]) != RSX_HOST_READY) continue;
            if (atomic_load(&c->seq[i]) >= q) continue;
            s = RSX_HOST_READY;
            atomic_compare_exchange_strong(&c->state[i], &s, RSX_HOST_FREE);
        }
        if (seq_out) *seq_out = q;
        return best;
    }
    return -1;
}

static inline void rsx_host_end_read(rsx_host_control* c, int slot)
{
    if (slot < 0 || slot >= RSX_HOST_SLOTS) return;
    uint32_t s = RSX_HOST_READING;
    atomic_compare_exchange_strong(&c->state[slot], &s, RSX_HOST_FREE);
}

/* A new game image attached: whatever the previous image left RENDERING will
 * never be published. */
static inline void rsx_host_reset_rendering(rsx_host_control* c)
{
    for (int i = 0; i < RSX_HOST_SLOTS; i++) {
        uint32_t s = RSX_HOST_RENDERING;
        atomic_compare_exchange_strong(&c->state[i], &s, RSX_HOST_FREE);
    }
}

#endif /* PS3RECOMP_RSX_HOST_SURFACE_H */

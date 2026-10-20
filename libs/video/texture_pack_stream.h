#ifndef FOLKLORE_TEXTURE_PACK_STREAM_H
#define FOLKLORE_TEXTURE_PACK_STREAM_H
#include "texture_pack.h"

/* Replacement artwork without render-thread stalls.
 *
 * Loading one replacement synchronously reads, inflates and verifies 21 MB
 * (512x512 source at 4x) to 85 MB (1024x1024) of RGBA: 0.1-0.6 s measured,
 * enough to freeze the game whenever an effect brings new textures. Here the
 * pack directory is listed once, so a texture without a replacement costs
 * only the hash of its base bytes; a replacement is loaded by worker threads
 * while the caller keeps drawing the original, and decoded replacements stay
 * in a byte-bounded LRU so the game evicting or rewriting a texture does not
 * read it from disk again.
 *
 * FOLKLORE_TEXTURE_PACK_CACHE_MB bounds the decoded cache (default 2048). */

enum { TEXTURE_PACK_NONE = 0, TEXTURE_PACK_READY = 1, TEXTURE_PACK_PENDING = 2 };

/* READY: *out addresses the cached levels and holds a reference, given back
 * with texture_pack_stream_release(*ticket) (never texture_pack_free).
 * PENDING: a worker is loading it; ask texture_pack_stream_poll(*ticket)
 * later. NONE: no replacement, or it failed validation. */
int texture_pack_stream_request(const char *root, const void *base, size_t base_len,
                                uint32_t format, uint32_t sw, uint32_t sh,
                                uint32_t *ticket, texture_pack *out);
int texture_pack_stream_poll(uint32_t ticket, texture_pack *out);
void texture_pack_stream_release(uint32_t ticket);
/* Stops the workers and frees the cache. */
void texture_pack_stream_shutdown(void);

typedef struct texture_pack_stream_stats {
    uint64_t requests, absent, hits, loads, failures, evictions;
    uint64_t cached_bytes, load_ns_total, load_ns_max;
} texture_pack_stream_stats;
void texture_pack_stream_get_stats(texture_pack_stream_stats *stats);
#endif

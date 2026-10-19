#ifndef FOLKLORE_TEXTURE_PACK_H
#define FOLKLORE_TEXTURE_PACK_H
#include <stddef.h>
#include <stdint.h>

/* FTX4, 104-byte little-endian header; packed RGBA8 mips, base first.
 * V1 stores raw mip bytes at offset 104. V2 stores one zlib stream there;
 * offset 64 is the uncompressed mip byte count in both versions and offset 72
 * is its SHA256. The returned level pointers always address decompressed RGBA.
 * Filename: <lowercase SHA256>-f<1|2|3>-<source_w>x<source_h>.frp.
 * Format 1/2/3 binds BC1/BC2/BC3 original base compressed bytes.
 * All fields are validated before the caller creates a backend texture.
 */
typedef struct texture_pack {
    uint8_t *file;
    uint32_t width, height, levels;
    const uint8_t *level[16];
    uint32_t level_width[16], level_height[16], row_bytes[16];
} texture_pack;

/* Returns 1 only for a fully validated replacement. Zero means original path. */
int texture_pack_load(const char *root, const void *base, size_t base_len,
                      uint32_t source_format, uint32_t source_w,
                      uint32_t source_h, texture_pack *out);
void texture_pack_free(texture_pack *pack);
void texture_pack_sha256(const void *data, size_t len, uint8_t digest[32]);
#endif

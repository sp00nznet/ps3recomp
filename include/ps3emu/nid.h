/*
 * ps3recomp - NID (Name ID) system
 *
 * PS3 uses a SHA-1 hash of (function_name + suffix) truncated to 32 bits as
 * the "NID" (Name IDentifier) that links imports to exports across PRX modules.
 *
 * The standard suffix appended before hashing is the empty string for most
 * firmware versions, but some libraries use a library-specific salt.  The
 * default suffix used in early firmware and for homebrew is "\x67\x59\x65\x99"
 * (the first 4 bytes of SHA-1("") with a twist).  In practice the NID database
 * is precomputed and shipped as a lookup table.
 */

#ifndef PS3EMU_NID_H
#define PS3EMU_NID_H

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Minimal SHA-1 (RFC 3174) -- only used for NID computation; not for crypto.
 * -----------------------------------------------------------------------*/

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3_sha1_ctx {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} ps3_sha1_ctx;

static inline uint32_t ps3_sha1_rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static inline void ps3_sha1_init(ps3_sha1_ctx* ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
}

static inline void ps3_sha1_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    for (i = 16; i < 80; i++)
        w[i] = ps3_sha1_rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);       k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        uint32_t tmp = ps3_sha1_rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ps3_sha1_rotl(b, 30); b = a; a = tmp;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static inline void ps3_sha1_update(ps3_sha1_ctx* ctx, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->count += len;

    while (len > 0) {
        size_t avail = 64 - idx;
        size_t chunk = len < avail ? len : avail;
        memcpy(ctx->buffer + idx, p, chunk);
        p += chunk; len -= chunk; idx += chunk;
        if (idx == 64) {
            ps3_sha1_transform(ctx->state, ctx->buffer);
            idx = 0;
        }
    }
}

static inline void ps3_sha1_final(ps3_sha1_ctx* ctx, uint8_t digest[20])
{
    uint64_t bits = ctx->count * 8;
    uint8_t pad = 0x80;
    ps3_sha1_update(ctx, &pad, 1);
    pad = 0;
    while ((ctx->count & 63) != 56)
        ps3_sha1_update(ctx, &pad, 1);

    uint8_t len_be[8];
    for (int i = 0; i < 8; i++)
        len_be[i] = (uint8_t)(bits >> (56 - i * 8));
    ps3_sha1_update(ctx, len_be, 8);

    for (int i = 0; i < 5; i++) {
        digest[i*4+0] = (uint8_t)(ctx->state[i] >> 24);
        digest[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

/* ---------------------------------------------------------------------------
 * NID computation
 * -----------------------------------------------------------------------*/

/* Default NID suffix (used by most firmware libraries).
 * This is the standard 16-byte suffix used in PS3 firmware NID computation.
 * Earlier documentation incorrectly listed only the first 4 bytes. */
#define PS3_NID_SUFFIX      "\x67\x59\x65\x99\x04\x25\x04\x90\x56\x64\x27\x49\x94\x89\x74\x1A"
#define PS3_NID_SUFFIX_LEN  16

/*
 * Compute the 32-bit NID for a function or variable name.
 * NID = first 4 bytes of SHA-1(name + suffix), interpreted little-endian.
 *
 * This matches the NID values found in PS3 ELF import/export tables as
 * used by the Cell OS Lv-2 linker and RPCS3.
 */
static inline uint32_t ps3_compute_nid(const char* name)
{
    ps3_sha1_ctx ctx;
    uint8_t digest[20];

    ps3_sha1_init(&ctx);
    ps3_sha1_update(&ctx, name, strlen(name));
    ps3_sha1_update(&ctx, PS3_NID_SUFFIX, PS3_NID_SUFFIX_LEN);
    ps3_sha1_final(&ctx, digest);

    /* Little-endian interpretation of first 4 bytes */
    return ((uint32_t)digest[0])       |
           ((uint32_t)digest[1] << 8)  |
           ((uint32_t)digest[2] << 16) |
           ((uint32_t)digest[3] << 24);
}

/*
 * Compute NID with a custom suffix (for libraries that use a non-default salt).
 */
static inline uint32_t ps3_compute_nid_ex(const char* name, const void* suffix, size_t suffix_len)
{
    ps3_sha1_ctx ctx;
    uint8_t digest[20];

    ps3_sha1_init(&ctx);
    ps3_sha1_update(&ctx, name, strlen(name));
    ps3_sha1_update(&ctx, suffix, suffix_len);
    ps3_sha1_final(&ctx, digest);

    /* Little-endian interpretation of first 4 bytes */
    return ((uint32_t)digest[0])       |
           ((uint32_t)digest[1] << 8)  |
           ((uint32_t)digest[2] << 16) |
           ((uint32_t)digest[3] << 24);
}

/* ---------------------------------------------------------------------------
 * NID lookup table
 * -----------------------------------------------------------------------*/

typedef struct ps3_nid_entry {
    uint32_t    nid;
    const char* name;       /* original function/variable name (may be NULL) */
    void*       handler;    /* HLE function pointer or stub */
} ps3_nid_entry;

typedef struct ps3_nid_table {
    ps3_nid_entry*  entries;
    uint32_t        count;
    uint32_t        capacity;
} ps3_nid_table;

static inline void ps3_nid_table_init(ps3_nid_table* t, ps3_nid_entry* storage, uint32_t cap)
{
    t->entries  = storage;
    t->count    = 0;
    t->capacity = cap;
}

static inline int ps3_nid_table_add(ps3_nid_table* t, uint32_t nid, const char* name, void* handler)
{
    if (t->count >= t->capacity) return -1;
    t->entries[t->count].nid     = nid;
    t->entries[t->count].name    = name;
    t->entries[t->count].handler = handler;
    t->count++;
    return 0;
}

static inline ps3_nid_entry* ps3_nid_table_find(const ps3_nid_table* t, uint32_t nid)
{
    for (uint32_t i = 0; i < t->count; i++) {
        if (t->entries[i].nid == nid)
            return &t->entries[i];
    }
    return NULL;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ---------------------------------------------------------------------------
 * C++ registration macros (used by module.h)
 * -----------------------------------------------------------------------*/
#ifdef __cplusplus

/*
 * REG_FNID(module, nid, func)  -- Register handler for a known NID.
 * REG_FUNC(module, func)       -- Register handler; NID is computed from name.
 */
#define REG_FNID(module_obj, nid_val, func_ptr) \
    ps3_nid_table_add(&(module_obj).func_table, (nid_val), #func_ptr, (void*)(func_ptr))

#define REG_FUNC(module_obj, func_ptr) \
    ps3_nid_table_add(&(module_obj).func_table, ps3_compute_nid(#func_ptr), #func_ptr, (void*)(func_ptr))

#define REG_VAR_FNID(module_obj, nid_val, var_ptr) \
    ps3_nid_table_add(&(module_obj).var_table, (nid_val), #var_ptr, (void*)(var_ptr))

#define REG_VAR(module_obj, var_ptr) \
    ps3_nid_table_add(&(module_obj).var_table, ps3_compute_nid(#var_ptr), #var_ptr, (void*)(var_ptr))

#endif /* __cplusplus */

#endif /* PS3EMU_NID_H */

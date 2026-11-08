/* edat.c -- NPDRM (EDAT/SDAT) decryption. See edat.h for why this lives in the FS layer.
 *
 * The algorithm is the published one (RPCS3's Crypto/unedat.cpp is the clearest
 * write-up); the structure below follows it closely so the two can be diffed:
 *
 *   header  0x00  NPD: magic, version, license, type, content_id[0x30],
 *                      digest[0x10], title_hash[0x10], dev_hash[0x10], times
 *           0x80  EDAT: flags, block_size, file_size
 *   0x100         per-block metadata (0x10 bytes each: the block's hash)
 *   then          the encrypted blocks
 *
 * Per block: the block key is dev_hash[0..0xB] + the big-endian block index, AES-ECB
 * encrypted under the file key; the result is both the CBC key for the data and the
 * CMAC key for the block hash. The CBC IV is the NPD digest (version >= 2).
 *
 * AES-128 and AES-CMAC are implemented here rather than pulled in, so the runtime
 * keeps no crypto dependency. edat_selftest() checks both against their standard
 * vectors before anything is decrypted -- see the note in edat.h.
 */

#include "edat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#endif

/* ---------------------------------------------------------------------------
 * AES-128
 * -----------------------------------------------------------------------*/

static const uint8_t AES_SBOX[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static uint8_t AES_ISBOX[256];
static int     s_isbox_ready = 0;

static void aes_build_isbox(void)
{
    if (s_isbox_ready) return;
    for (int i = 0; i < 256; i++) AES_ISBOX[AES_SBOX[i]] = (uint8_t)i;
    s_isbox_ready = 1;
}

static uint8_t xtime(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

/* 11 round keys of 16 bytes. */
static void aes128_expand(const uint8_t key[16], uint8_t rk[176])
{
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, rk + i - 4, 4);
        if ((i % 16) == 0) {
            uint8_t tmp = t[0];
            t[0] = (uint8_t)(AES_SBOX[t[1]] ^ rcon);
            t[1] = AES_SBOX[t[2]];
            t[2] = AES_SBOX[t[3]];
            t[3] = AES_SBOX[tmp];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++) rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ t[j]);
    }
}

static void aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = AES_SBOX[s[i]];
        /* ShiftRows (state is column-major: byte i is row i%4, column i/4). */
        uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[c * 4 + r] = s[((c + r) % 4) * 4 + r];
        memcpy(s, t, 16);
        if (round != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t* p = s + c * 4;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                p[1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                p[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                p[3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
    }
    memcpy(out, s, 16);
}

static void aes128_decrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16])
{
    aes_build_isbox();
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[160 + i]);
    for (int round = 9; round >= 0; round--) {
        /* InvShiftRows */
        uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[((c + r) % 4) * 4 + r] = s[c * 4 + r];
        memcpy(s, t, 16);
        for (int i = 0; i < 16; i++) s[i] = AES_ISBOX[s[i]];
        for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
        if (round != 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t* p = s + c * 4;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = (uint8_t)(gmul(a0,14) ^ gmul(a1,11) ^ gmul(a2,13) ^ gmul(a3, 9));
                p[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1,14) ^ gmul(a2,11) ^ gmul(a3,13));
                p[2] = (uint8_t)(gmul(a0,13) ^ gmul(a1, 9) ^ gmul(a2,14) ^ gmul(a3,11));
                p[3] = (uint8_t)(gmul(a0,11) ^ gmul(a1,13) ^ gmul(a2, 9) ^ gmul(a3,14));
            }
        }
    }
    memcpy(out, s, 16);
}

static void aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                               const uint8_t* in, uint8_t* out, size_t len)
{
    uint8_t rk[176];
    uint8_t prev[16], cur[16];
    aes128_expand(key, rk);
    memcpy(prev, iv, 16);
    for (size_t off = 0; off + 16 <= len; off += 16) {
        memcpy(cur, in + off, 16);
        aes128_decrypt_block(rk, cur, out + off);
        for (int i = 0; i < 16; i++) out[off + i] ^= prev[i];
        memcpy(prev, cur, 16);
    }
}

/* AES-CMAC (RFC 4493). */
static void cmac_shift_left(const uint8_t in[16], uint8_t out[16])
{
    uint8_t carry = 0;
    for (int i = 15; i >= 0; i--) {
        uint8_t b = in[i];
        out[i] = (uint8_t)((b << 1) | carry);
        carry = (uint8_t)((b >> 7) & 1);
    }
}

static void aes128_cmac(const uint8_t key[16], const uint8_t* msg, size_t len, uint8_t mac[16])
{
    uint8_t rk[176], zero[16] = {0}, L[16], K1[16], K2[16], X[16] = {0}, blk[16];
    aes128_expand(key, rk);
    aes128_encrypt_block(rk, zero, L);
    cmac_shift_left(L, K1);  if (L[0]  & 0x80) K1[15] ^= 0x87;
    cmac_shift_left(K1, K2); if (K1[0] & 0x80) K2[15] ^= 0x87;

    size_t n = (len + 15) / 16;
    int complete = (n != 0) && (len % 16 == 0);
    if (n == 0) n = 1;

    for (size_t i = 0; i < n - 1; i++) {
        for (int j = 0; j < 16; j++) X[j] ^= msg[i * 16 + j];
        aes128_encrypt_block(rk, X, X);
    }
    if (complete) {
        memcpy(blk, msg + (n - 1) * 16, 16);
        for (int j = 0; j < 16; j++) blk[j] ^= K1[j];
    } else {
        size_t rem = len % 16;
        memset(blk, 0, 16);
        if (rem) memcpy(blk, msg + (n - 1) * 16, rem);
        blk[rem] = 0x80;
        for (int j = 0; j < 16; j++) blk[j] ^= K2[j];
    }
    for (int j = 0; j < 16; j++) X[j] ^= blk[j];
    aes128_encrypt_block(rk, X, mac);
}

/* ---------------------------------------------------------------------------
 * NPDRM constants (the published set)
 * -----------------------------------------------------------------------*/
/* Candidate klicensees for content not bound to a per-console RAP.
 *
 * Which one applies is not guessed: the NPD header carries dev_hash, which is
 * CMAC(klic ^ NP_OMAC_KEY_2) over its own first 0x60 bytes, so the file itself says
 * which key is right. Trying them and keeping the one that verifies is simpler than
 * branching on license type and strictly safer -- a key that does not verify is
 * never used.
 *
 * Worth knowing: a PSOne Classic uses NP_PSX_KEY for BOTH license type 3 ("free")
 * and license type 2 (retail), so an ordinary retail ISO.BIN.EDAT decrypts with no
 * RAP at all. */
/* ---------------------------------------------------------------------------
 * SHA-1 / HMAC-SHA1
 *
 * Needed for the EDAT hash modes. Only AES-CMAC was here before, which covers
 * hash mode 0x02; modes 0x01 and 0x04 are HMAC-SHA1 (with a 0x14- and a
 * 0x10-byte key respectively) and a file using either could not be checked --
 * so those flag combinations were refused outright rather than mis-verified.
 * -----------------------------------------------------------------------*/

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_ctx;

static uint32_t rol32(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }

static void sha1_block(sha1_ctx* c, const uint8_t* p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3], f = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t k, t;
        if      (i < 20) { t = (b & d) | (~b & e);            k = 0x5A827999u; }
        else if (i < 40) { t = b ^ d ^ e;                     k = 0x6ED9EBA1u; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e);   k = 0x8F1BBCDCu; }
        else             { t = b ^ d ^ e;                     k = 0xCA62C1D6u; }
        uint32_t tmp = rol32(a, 5) + t + f + k + w[i];
        f = e; e = d; d = rol32(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

static void sha1_init(sha1_ctx* c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0; c->n = 0;
}

static void sha1_update(sha1_ctx* c, const uint8_t* p, size_t n)
{
    c->len += n;
    while (n) {
        size_t take = 64 - c->n;
        if (take > n) take = n;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}

static void sha1_final(sha1_ctx* c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) sha1_update(c, &pad, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - i*8));
    sha1_update(c, l, 8);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24); out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);  out[i*4+3] = (uint8_t)c->h[i];
    }
}

/* HMAC-SHA1. The EDAT key lengths (0x10, 0x14) are both under the 64-byte block
 * size, so the key is only zero-padded, never hashed down. */
static void hmac_sha1(const uint8_t* key, size_t keylen,
                      const uint8_t* msg, size_t msglen, uint8_t out[20])
{
    uint8_t k[64] = {0}, ipad[64], opad[64], inner[20];
    if (keylen > 64) { sha1_ctx t; sha1_init(&t); sha1_update(&t, key, keylen); sha1_final(&t, k); }
    else             memcpy(k, key, keylen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }

    sha1_ctx c;
    sha1_init(&c); sha1_update(&c, ipad, 64); sha1_update(&c, msg, msglen); sha1_final(&c, inner);
    sha1_init(&c); sha1_update(&c, opad, 64); sha1_update(&c, inner, 20);   sha1_final(&c, out);
}

/* The per-block ERK/hash keys an EDAT wraps its own block keys under. Not
 * title-specific and not a license: they are the same two constants for every
 * EDAT on the platform, and useless without the file's own key material. */
static const uint8_t EDAT_KEY_0[16] = {
    0xBE,0x95,0x9C,0xA8,0x30,0x8D,0xEF,0xA2,0xE5,0xE1,0x80,0xC6,0x37,0x12,0xA9,0xAE };
static const uint8_t EDAT_KEY_1[16] = {
    0x4C,0xA9,0xC1,0x4B,0x01,0xC9,0x53,0x09,0x96,0x9B,0xEC,0x68,0xAA,0x0B,0xC0,0x81 };

static const struct { const char* name; uint8_t key[16]; } KLIC_CANDIDATES[] = {
    { "NP_PSX_KEY",   { 0x52,0xC0,0xB5,0xCA,0x76,0xD6,0x13,0x4B,
                        0xB4,0x5F,0xC6,0x6C,0xA6,0x37,0xF2,0xC1 } },  /* PSOne Classics */
    { "NP_KLIC_FREE", { 0x72,0xF9,0x90,0x78,0x8F,0x9C,0xFF,0x74,
                        0x57,0x25,0xF0,0x8E,0x4C,0x12,0x83,0x87 } },  /* free content   */
    { "NP_PSP_KEY_1", { 0x2A,0x6A,0xFB,0xCF,0x43,0xD1,0x57,0x9F,
                        0x7D,0x73,0x87,0x41,0xA1,0x3B,0xD4,0x2E } },  /* PSP Minis      */
    { "NP_PSP_KEY_2", { 0x0D,0xB8,0x57,0x32,0x36,0x6C,0xD7,0x34,
                        0xFC,0x87,0x9E,0x74,0x33,0x43,0xBB,0x4F } },  /* PSP Remasters  */
};

/* Klicensees the guest passed to sceNpDrmIsAvailable[2] (edat_add_klicensee). */
#define EDAT_GUEST_KLIC_MAX 16
static uint8_t s_guest_klic[EDAT_GUEST_KLIC_MAX][16];
static int     s_guest_klic_n = 0;

void edat_add_klicensee(const uint8_t klic[16])
{
    if (!klic) return;
    for (int i = 0; i < s_guest_klic_n; i++)
        if (memcmp(s_guest_klic[i], klic, 16) == 0) return;
    if (s_guest_klic_n >= EDAT_GUEST_KLIC_MAX) {
        fprintf(stderr, "[edat] klicensee table full -- ignoring a new key\n");
        return;
    }
    memcpy(s_guest_klic[s_guest_klic_n++], klic, 16);
    fprintf(stderr, "[edat] guest klicensee #%d registered\n", s_guest_klic_n);
}

static const uint8_t NP_OMAC_KEY_2[16] = {
    0x6B,0xA5,0x29,0x76,0xEF,0xDA,0x16,0xEF,0x3C,0x33,0x9F,0xB2,0x97,0x1E,0x25,0x6B };
static const uint8_t SDAT_KEY[16] = {
    0x0D,0x65,0x5E,0xF8,0xE6,0x74,0xA9,0x8A,0xB8,0x50,0x5C,0xFA,0x7D,0x01,0x29,0x33 };

#define SDAT_FLAG              0x01000000u
#define EDAT_COMPRESSED_FLAG   0x00000001u
#define EDAT_FLAG_0x02         0x00000002u
#define EDAT_ENCRYPTED_KEY_FLAG 0x00000008u
#define EDAT_FLAG_0x10         0x00000010u
#define EDAT_FLAG_0x20         0x00000020u
#define EDAT_DEBUG_DATA_FLAG   0x80000000u

static uint32_t be32(const uint8_t* p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

/* ---------------------------------------------------------------------------
 * Self-test
 * -----------------------------------------------------------------------*/
int edat_selftest(void)
{
    /* FIPS-197 AES-128 */
    static const uint8_t k[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                                  0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static const uint8_t p[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                  0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    static const uint8_t c[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                                  0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    uint8_t rk[176], out[16];
    aes128_expand(k, rk);
    aes128_encrypt_block(rk, p, out);
    if (memcmp(out, c, 16) != 0) return -1;
    aes128_decrypt_block(rk, c, out);
    if (memcmp(out, p, 16) != 0) return -2;

    /* RFC 4493 AES-CMAC, empty message */
    static const uint8_t ck[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
                                   0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
    static const uint8_t cm[16] = {0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28,
                                   0x7f,0xa3,0x7d,0x12,0x9b,0x75,0x67,0x46};
    uint8_t mac[16];
    aes128_cmac(ck, NULL, 0, mac);
    if (memcmp(mac, cm, 16) != 0) return -3;

    /* FIPS-180 SHA-1("abc") */
    static const uint8_t sv[20] = {0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
                                   0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    uint8_t sd[20]; sha1_ctx sc;
    sha1_init(&sc); sha1_update(&sc, (const uint8_t*)"abc", 3); sha1_final(&sc, sd);
    if (memcmp(sd, sv, 20) != 0) return -4;

    /* RFC 2202 HMAC-SHA1 case 1 */
    static const uint8_t hk[20] = {0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
                                   0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b};
    static const uint8_t hv[20] = {0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,
                                   0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00};
    uint8_t hd[20];
    hmac_sha1(hk, 20, (const uint8_t*)"Hi There", 8, hd);
    if (memcmp(hd, hv, 20) != 0) return -5;
    return 0;
}

/* ---------------------------------------------------------------------------
 * EDAT
 * -----------------------------------------------------------------------*/
int edat_is_npd(const char* host_path)
{
    FILE* f = fopen(host_path, "rb");
    if (!f) return 0;
    uint8_t m[4] = {0};
    size_t n = fread(m, 1, 4, f);
    fclose(f);
    return n == 4 && m[0] == 'N' && m[1] == 'P' && m[2] == 'D' && m[3] == 0;
}

/* The RIF key for a license-type-1/2 EDAT: $PS3_RIF_KEY as 32 hex chars, else
 * the 16 raw bytes of a <file>.rifkey sidecar. This is the buyer's own license
 * material for one content id -- deriving it from a .rap is deliberately out of
 * scope here, so nothing in this tree has to carry license-conversion keys. */
static int edat_rif_key(const char* in_path, uint8_t out[16])
{
    const char* e = getenv("PS3_RIF_KEY");
    if (e) {
        int n = 0;
        for (; n < 16 && e[n*2] && e[n*2+1]; n++) {
            char b[3] = { e[n*2], e[n*2+1], 0 };
            char* end = NULL;
            long v = strtol(b, &end, 16);
            if (end != b + 2) break;
            out[n] = (uint8_t)v;
        }
        if (n == 16) return 1;
        fprintf(stderr, "[edat] PS3_RIF_KEY is not 32 hex chars -- ignoring\n");
    }
    char side[1100];
    if ((size_t)snprintf(side, sizeof side, "%s.rifkey", in_path) >= sizeof side) return 0;
    FILE* f = fopen(side, "rb");
    if (!f) return 0;
    size_t n = fread(out, 1, 16, f);
    fclose(f);
    if (n == 16) { fprintf(stderr, "[edat] RIF key from %s\n", side); return 1; }
    return 0;
}

int edat_decrypt_file(const char* in_path, const char* out_path)
{
    static int checked = 0;
    if (!checked) {
        int rc = edat_selftest();
        checked = 1;
        if (rc != 0) {
            fprintf(stderr, "[edat] SELF-TEST FAILED (%d) -- refusing to decrypt; the "
                            "AES/CMAC primitives are wrong and would silently produce "
                            "garbage\n", rc);
            return -100;
        }
    }

    FILE* in = fopen(in_path, "rb");
    if (!in) return -1;

    uint8_t hdr[0x100];
    if (fread(hdr, 1, sizeof hdr, in) != sizeof hdr) { fclose(in); return -2; }

    uint32_t version  = be32(hdr + 0x04);
    uint32_t license  = be32(hdr + 0x08);
    const uint8_t* digest   = hdr + 0x40;
    const uint8_t* dev_hash = hdr + 0x60;
    uint32_t flags      = be32(hdr + 0x80);
    uint32_t block_size = be32(hdr + 0x84);
    uint64_t file_size  = be64(hdr + 0x88);

    fprintf(stderr, "[edat] %s: version=%u license=%u flags=0x%08X block=0x%X size=%llu\n",
            in_path, version, license, flags, block_size,
            (unsigned long long)file_size);

    if (flags & (EDAT_COMPRESSED_FLAG | EDAT_DEBUG_DATA_FLAG)) {
        fprintf(stderr, "[edat] unsupported flags 0x%08X (per-block zlib compression "
                        "and debug data are not implemented)\n", flags);
        fclose(in); return -3;
    }
    if (block_size == 0 || block_size > (16u << 20)) { fclose(in); return -4; }

    /* The file key.
     *
     *   SDAT             derived from the header alone.
     *   license type 3   the klicensee itself ("free" content).
     *   license type 1/2 the RIF key. The klicensee still verifies against
     *                    dev_hash for these -- dev_hash authenticates the
     *                    HEADER -- but the DATA is under the RIF key, so a
     *                    dev_hash match alone does NOT mean decryptable. That
     *                    cost a debugging round twice: a retail PSOne
     *                    ISO.BIN.EDAT validates under NP_PSX_KEY and then fails
     *                    every block hash, and LittleBigPlanet's data.edat
     *                    validates under a klicensee baked into its own EBOOT
     *                    and likewise decrypts to noise.
     *
     * The RIF key is per-buyer license material, so it is never built in: it
     * comes from the operator, via $PS3_RIF_KEY (32 hex chars) or a <file>.rifkey
     * sidecar. Whether it is right is not taken on trust -- a wrong key fails
     * block 0's hash below and nothing is written. */
    uint8_t key[16];
    int have_key = 0;
    if (flags & SDAT_FLAG) {
        for (int i = 0; i < 16; i++) key[i] = (uint8_t)(dev_hash[i] ^ SDAT_KEY[i]);
        fprintf(stderr, "[edat] SDAT: key derived from dev_hash\n");
        have_key = 1;
    } else {
        /* dev_hash = AES-CMAC(klic ^ NP_OMAC_KEY_2) over header bytes 0x00..0x5F.
         * The check is the same for NPD versions 2, 3 and 4 (version 3 only adds
         * the metadata ECDSA signature, which a reader may skip). Keys from the
         * guest first: a title's own klicensee is the common case for game data,
         * the built-in ones only cover the platform-wide free/PSX/PSP keys. */
        const int n_builtin = (int)(sizeof KLIC_CANDIDATES / sizeof KLIC_CANDIDATES[0]);
        int found = -1;
        const uint8_t* found_key = NULL;
        for (int c = 0; c < s_guest_klic_n + n_builtin && found < 0; c++) {
            const uint8_t* k = (c < s_guest_klic_n) ? s_guest_klic[c]
                                                    : KLIC_CANDIDATES[c - s_guest_klic_n].key;
            uint8_t ck[16], mac[16];
            for (int i = 0; i < 16; i++) ck[i] = (uint8_t)(k[i] ^ NP_OMAC_KEY_2[i]);
            aes128_cmac(ck, hdr, 0x60, mac);
            if (memcmp(mac, dev_hash, 16) == 0) { found = c; found_key = k; }
        }
        if (found >= 0 && (license & 3) == 3) {
            memcpy(key, found_key, 16);
            have_key = 1;
            if (found < s_guest_klic_n)
                fprintf(stderr, "[edat] guest klicensee #%d verified against dev_hash\n", found + 1);
            else
                fprintf(stderr, "[edat] klicensee %s verified against dev_hash\n",
                        KLIC_CANDIDATES[found - s_guest_klic_n].name);
        } else if (edat_rif_key(in_path, key)) {
            have_key = 1;
            fprintf(stderr, "[edat] license type %u: using the supplied RIF key%s\n",
                    license, found >= 0 ? "" : " (dev_hash names an unknown klicensee)");
        }
        if (!have_key && (license & 3) == 3) {
            fprintf(stderr,
                "[edat] license type 3 (free): no known klicensee verifies dev_hash.\n"
                "[edat]   The title's klicensee comes from its own sceNpDrmIsAvailable[2]\n"
                "[edat]   call (edat_add_klicensee); open the file after that call.\n");
            fclose(in); return -6;
        }
        if (!have_key) {
            fprintf(stderr,
                "[edat] license type %u needs a RIF key and none was supplied.\n"
                "[edat]   set PS3_RIF_KEY=<32 hex chars>, or put the 16 raw bytes in\n"
                "[edat]   %s.rifkey -- it is derived from your own .rap for this\n"
                "[edat]   content id, which this runtime deliberately cannot do for you.\n",
                license, in_path);
            fclose(in); return -6;
        }
    }

    uint32_t total_blocks = (uint32_t)((file_size + block_size - 1) / block_size);

    /* Layout. With FLAG_0x20 (or compression) each block carries a 0x20-byte
     * metadata section IMMEDIATELY BEFORE its data; otherwise the 0x10-byte
     * sections form one table at 0x100 and the data follows all of them. */
    const uint32_t meta_size = (flags & (EDAT_COMPRESSED_FLAG | EDAT_FLAG_0x20)) ? 0x20 : 0x10;
    const int      meta_leads = (flags & EDAT_FLAG_0x20) != 0;
    const uint64_t data_base  = 0x100 + (uint64_t)total_blocks * meta_size;

    /* Crypto/hash mode, exactly as the flags select it.
     *   ENCRYPTED_KEY (0x08): the per-block ERK is itself wrapped under EDAT_KEY.
     *   0x10 clear          -> hash is AES-CMAC over a 0x10-byte key
     *   0x10 set, 0x20 clear-> HMAC-SHA1 over a 0x10-byte key
     *   0x10 set, 0x20 set  -> HMAC-SHA1 over a 0x14-byte key
     * The 0x14-byte case only ever fills the first 0x10; the tail stays zero. */
    const int enc_key  = (flags & EDAT_ENCRYPTED_KEY_FLAG) != 0;
    const int hash_alg = !(flags & EDAT_FLAG_0x10) ? 2 : (!(flags & EDAT_FLAG_0x20) ? 4 : 1);
    const uint8_t* erk = (version == 4) ? EDAT_KEY_1 : EDAT_KEY_0;

    FILE* out = fopen(out_path, "wb");
    if (!out) { fclose(in); return -7; }

    uint8_t* enc = (uint8_t*)malloc(block_size + 16);
    uint8_t* dec = (uint8_t*)malloc(block_size + 16);
    if (!enc || !dec) { free(enc); free(dec); fclose(in); fclose(out); return -8; }

    uint8_t rk_file[176];
    aes128_expand(key, rk_file);

    int rc = 0;
    uint64_t written = 0;
    for (uint32_t b = 0; b < total_blocks; b++) {
        uint64_t meta_off = meta_leads
            ? 0x100 + (uint64_t)b * (meta_size + block_size)
            : 0x100 + (uint64_t)b * meta_size;
        uint64_t data_off = meta_leads ? meta_off + meta_size
                                       : data_base + (uint64_t)b * block_size;

        uint8_t meta[0x20];
        if (fseek(in, (long)meta_off, SEEK_SET) != 0 ||
            fread(meta, 1, meta_size, in) != meta_size) { rc = -9; break; }

        /* With FLAG_0x20 the stored hash is the xor of the two halves. */
        uint8_t block_hash[16];
        if (meta_size == 0x20)
            for (int k = 0; k < 16; k++) block_hash[k] = (uint8_t)(meta[k] ^ meta[k + 16]);
        else
            memcpy(block_hash, meta, 16);

        uint64_t len = block_size;
        if (b == total_blocks - 1 && (file_size % block_size))
            len = file_size % block_size;
        uint64_t padded = (len + 15) & ~(uint64_t)15;

        if (fseek(in, (long)data_off, SEEK_SET) != 0 ||
            fread(enc, 1, (size_t)padded, in) != padded) { rc = -10; break; }

        /* Block key. Version 0/1 seeds from zero and uses a zero CBC IV;
         * version 2+ seeds from dev_hash and uses the NPD digest as the IV.
         * Getting any of it wrong fails the block hash rather than producing
         * garbage, which is exactly why the hash is checked. */
        uint8_t bk[16] = {0}, kres[16];
        if (version > 1) memcpy(bk, dev_hash, 12);
        bk[12] = (uint8_t)(b >> 24); bk[13] = (uint8_t)(b >> 16);
        bk[14] = (uint8_t)(b >> 8);  bk[15] = (uint8_t)b;
        aes128_encrypt_block(rk_file, bk, kres);

        /* The hash seed is the block key run through the file key a second
         * time when 0x10 is set. */
        uint8_t hseed[16];
        if (flags & EDAT_FLAG_0x10) aes128_encrypt_block(rk_file, kres, hseed);
        else                        memcpy(hseed, kres, 16);

        /* An encrypted ERK unwraps the CBC key and the hash key under EDAT_KEY
         * (note: EDAT_KEY, not EDAT_HASH -- the hash constants are only used by
         * the default-ERK mode, which nothing here selects). The IV is taken
         * as-is either way. */
        static const uint8_t zero_iv[16] = {0};
        uint8_t ckey[16], hkey[20] = {0};
        if (enc_key) {
            aes128_cbc_decrypt(erk, zero_iv, kres,  ckey, 16);
            aes128_cbc_decrypt(erk, zero_iv, hseed, hkey, 16);
        } else {
            memcpy(ckey, kres, 16);
            memcpy(hkey, hseed, 16);
        }

        uint8_t mac[20];
        if (hash_alg == 2) aes128_cmac(hkey, enc, (size_t)padded, mac);
        else               hmac_sha1(hkey, (hash_alg == 1) ? 20 : 16,
                                     enc, (size_t)padded, mac);
        if (memcmp(mac, block_hash, 16) != 0) {
            fprintf(stderr, "[edat] block %u hash mismatch -- wrong key or corrupt "
                            "file; stopping rather than writing garbage\n", b);
            rc = -11; break;
        }

        aes128_cbc_decrypt(ckey, (version <= 1) ? zero_iv : digest,
                           enc, dec, (size_t)padded);
        if (fwrite(dec, 1, (size_t)len, out) != len) { rc = -12; break; }
        written += len;
    }

    free(enc); free(dec);
    fclose(in);
    fclose(out);

    if (rc == 0)
        fprintf(stderr, "[edat] decrypted %llu bytes -> %s\n",
                (unsigned long long)written, out_path);
    else
        remove(out_path);   /* never leave a half-written cache behind */
    return rc;
}

/* SDATA: an NPD container whose EDAT header flags (big-endian at 0x80) carry
 * SDAT_FLAG. */
static int npd_is_sdata(const char* host_path)
{
    FILE* f = fopen(host_path, "rb");
    if (!f) return 0;
    uint8_t h[0x84] = {0};
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    return n == sizeof h && memcmp(h, "NPD\0", 4) == 0 && (be32(h + 0x80) & SDAT_FLAG) != 0;
}

int edat_is_sdata(const char* host_path)
{
    return host_path && npd_is_sdata(host_path);
}

/* Where the decrypted copy of `host_path` lives. Never beside the source: that is
 * usually the read-only disc tree (dev_bdvd), and a stray <file>.dec there is a
 * file the title never had. $PS3RECOMP_EDAT_CACHE names the directory; otherwise
 * a ps3recomp-edat directory under the host temp directory. The name keys the
 * full source path (FNV-1a 64) and keeps its base name for diagnostics. */
static int edat_cache_path(const char* host_path, char* buf, size_t cap)
{
    char dir[1024];
    const char* base_dir = getenv("PS3RECOMP_EDAT_CACHE");
    if (base_dir && *base_dir) {
        if ((size_t)snprintf(dir, sizeof dir, "%s", base_dir) >= sizeof dir) return -1;
    } else {
#ifdef _WIN32
        char tmp[MAX_PATH + 1];
        DWORD n = GetTempPathA((DWORD)sizeof tmp, tmp);
        if (n == 0 || n > sizeof tmp) return -1;
        if ((size_t)snprintf(dir, sizeof dir, "%sps3recomp-edat", tmp) >= sizeof dir) return -1;
#else
        const char* tmp = getenv("TMPDIR");
        if (!tmp || !*tmp) tmp = "/tmp";
        if ((size_t)snprintf(dir, sizeof dir, "%s/ps3recomp-edat", tmp) >= sizeof dir) return -1;
#endif
    }
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0700);
#endif
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char* p = (const unsigned char*)host_path; *p; p++) {
        unsigned char c = *p == '\\' ? '/' : *p;
        h = (h ^ c) * 1099511628211ull;
    }
    const char* name = host_path;
    for (const char* p = host_path; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    return (size_t)snprintf(buf, cap, "%s/%016llx-%s.dec", dir, (unsigned long long)h, name) >= cap ? -1 : 0;
}

const char* edat_resolve(const char* host_path, char* buf, size_t cap)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* e = getenv("PS3_EDAT");
        enabled = (e && e[0] == '0') ? 0 : 1;
    }
    if (!enabled || !host_path || !edat_is_npd(host_path)) return host_path;

    if (edat_cache_path(host_path, buf, cap) != 0) return host_path;

    struct stat ss, ds;
    if (stat(host_path, &ss) == 0 && stat(buf, &ds) == 0 && ds.st_mtime >= ss.st_mtime)
        return buf;                                    /* cache still valid */

    if (edat_decrypt_file(host_path, buf) != 0) return host_path;
    return buf;
}

/* A plain open (cellFsOpen, sys_fs_open). On hardware an EDAT reads back as
 * plaintext once the title has primed its key, but SDATA is decrypted only by
 * cellFsSdataOpen: a plain open of a .sdat returns the container's own bytes,
 * the same ones stat sizes. A title that copies its .sdat elsewhere reads it
 * raw, stat-size bytes; serving the shorter plaintext made that copy fail on
 * the final short read. */
const char* edat_resolve_open(const char* host_path, char* buf, size_t cap)
{
    if (edat_is_sdata(host_path)) return host_path;
    return edat_resolve(host_path, buf, cap);
}

/*
 * SDATA/EDAT (NPD) decryption for cellFsSdataOpen.
 *
 * PS3 titles store protected data (LBP: its SPU job code modules) in .sdat
 * files -- an NPD container encrypted per 0x4000-byte block with AES-128. SDATA
 * (the SDAT_FLAG variant) is self-decryptable with fixed keys, no NPDRM
 * license. Algorithm verified against RPCS3 Crypto/unedat.cpp and validated by
 * decrypting LBP's data.sdat to its plaintext "FSHb" container.
 *
 * Per block n:
 *   crypt_key  = dev_hash ^ SDAT_KEY
 *   b_key      = dev_hash[0:12] || be32(n)
 *   key_result = AES_ECB_encrypt(crypt_key, b_key)
 *   key_final  = (flags & 0x08 ENCRYPTED_KEY) ? AES_CBC_decrypt(EDAT_KEY_0, 0, key_result)
 *                                             : key_result
 *   plaintext  = AES_CBC_decrypt(key_final, iv=npd.digest, ciphertext)
 *
 * Self-contained (compact AES-128), header-only static inline.
 */
#ifndef SDATA_DECRYPT_H
#define SDATA_DECRYPT_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ---- compact AES-128 (public-domain style) ---------------------------- */
static const uint8_t SD_sbox[256] = {
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
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static uint8_t SD_rsbox[256];
static int SD_rsbox_init = 0;
static const uint8_t SD_rcon[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static inline uint8_t sd_xtime(uint8_t x){ return (uint8_t)((x<<1) ^ ((x>>7)*0x1b)); }
static inline uint8_t sd_mul(uint8_t x, uint8_t y){
    uint8_t r=0; for(int i=0;i<8;i++){ if(y&1) r^=x; uint8_t hi=x&0x80; x<<=1; if(hi) x^=0x1b; y>>=1;} return r;
}

static inline void sd_key_expand(const uint8_t key[16], uint8_t rk[176]){
    memcpy(rk, key, 16);
    for(int i=16;i<176;i+=4){
        uint8_t t[4]; memcpy(t, rk+i-4, 4);
        if(i%16==0){
            uint8_t tmp=t[0]; t[0]=SD_sbox[t[1]]^SD_rcon[i/16]; t[1]=SD_sbox[t[2]]; t[2]=SD_sbox[t[3]]; t[3]=SD_sbox[tmp];
        }
        for(int j=0;j<4;j++) rk[i+j]=rk[i-16+j]^t[j];
    }
}

static inline void sd_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]){
    uint8_t s[16]; memcpy(s,in,16);
    for(int i=0;i<16;i++) s[i]^=rk[i];
    for(int round=1;round<=10;round++){
        for(int i=0;i<16;i++) s[i]=SD_sbox[s[i]];                      /* SubBytes */
        uint8_t t; t=s[1];s[1]=s[5];s[5]=s[9];s[9]=s[13];s[13]=t;      /* ShiftRows */
        t=s[2];s[2]=s[10];s[10]=t; t=s[6];s[6]=s[14];s[14]=t;
        t=s[15];s[15]=s[11];s[11]=s[7];s[7]=s[3];s[3]=t;
        if(round<10){                                                  /* MixColumns */
            for(int c=0;c<4;c++){
                uint8_t *col=s+c*4; uint8_t a0=col[0],a1=col[1],a2=col[2],a3=col[3];
                col[0]=(uint8_t)(sd_xtime(a0)^(sd_xtime(a1)^a1)^a2^a3);
                col[1]=(uint8_t)(a0^sd_xtime(a1)^(sd_xtime(a2)^a2)^a3);
                col[2]=(uint8_t)(a0^a1^sd_xtime(a2)^(sd_xtime(a3)^a3));
                col[3]=(uint8_t)((sd_xtime(a0)^a0)^a1^a2^sd_xtime(a3));
            }
        }
        for(int i=0;i<16;i++) s[i]^=rk[round*16+i];                    /* AddRoundKey */
    }
    memcpy(out,s,16);
}

static inline void sd_decrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]){
    if(!SD_rsbox_init){ for(int i=0;i<256;i++) SD_rsbox[SD_sbox[i]]=(uint8_t)i; SD_rsbox_init=1; }
    uint8_t s[16]; memcpy(s,in,16);
    for(int i=0;i<16;i++) s[i]^=rk[160+i];
    for(int round=9;round>=0;round--){
        uint8_t t; t=s[13];s[13]=s[9];s[9]=s[5];s[5]=s[1];s[1]=t;      /* InvShiftRows */
        t=s[2];s[2]=s[10];s[10]=t; t=s[6];s[6]=s[14];s[14]=t;
        t=s[3];s[3]=s[7];s[7]=s[11];s[11]=s[15];s[15]=t;
        for(int i=0;i<16;i++) s[i]=SD_rsbox[s[i]];                     /* InvSubBytes */
        for(int i=0;i<16;i++) s[i]^=rk[round*16+i];                   /* AddRoundKey */
        if(round>0){                                                  /* InvMixColumns */
            for(int c=0;c<4;c++){
                uint8_t *col=s+c*4; uint8_t a0=col[0],a1=col[1],a2=col[2],a3=col[3];
                col[0]=sd_mul(a0,14)^sd_mul(a1,11)^sd_mul(a2,13)^sd_mul(a3,9);
                col[1]=sd_mul(a0,9)^sd_mul(a1,14)^sd_mul(a2,11)^sd_mul(a3,13);
                col[2]=sd_mul(a0,13)^sd_mul(a1,9)^sd_mul(a2,14)^sd_mul(a3,11);
                col[3]=sd_mul(a0,11)^sd_mul(a1,13)^sd_mul(a2,9)^sd_mul(a3,14);
            }
        }
    }
    memcpy(out,s,16);
}

static inline void sd_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                                  const uint8_t* in, uint8_t* out, size_t len){
    uint8_t rk[176]; sd_key_expand(key, rk);
    uint8_t prev[16]; memcpy(prev, iv, 16);
    for(size_t o=0;o+16<=len;o+=16){
        uint8_t blk[16], dec[16];
        memcpy(blk, in+o, 16);
        sd_decrypt_block(rk, blk, dec);
        for(int i=0;i<16;i++) out[o+i]=dec[i]^prev[i];
        memcpy(prev, blk, 16);
    }
}

/* Read a big-endian value. */
static inline uint32_t sd_be32(const uint8_t* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static inline uint64_t sd_be64(const uint8_t* p){ return ((uint64_t)sd_be32(p)<<32)|sd_be32(p+4); }

/* Decrypt an in-memory SDATA/EDAT (NPD) file. Returns malloc'd plaintext of
 * *out_size bytes, or NULL if not NPD / unsupported (EDAT needing a license).
 * Caller frees. */
static inline uint8_t* sdata_decrypt(const uint8_t* d, size_t n, size_t* out_size){
    static const uint8_t SDAT_KEY[16]  = {0x0D,0x65,0x5E,0xF8,0xE6,0x74,0xA9,0x8A,0xB8,0x50,0x5C,0xFA,0x7D,0x01,0x29,0x33};
    static const uint8_t EDAT_KEY_0[16]= {0xBE,0x95,0x9C,0xA8,0x30,0x8D,0xEF,0xA2,0xE5,0xE1,0x80,0xC6,0x37,0x12,0xA9,0xAE};
    static const uint8_t ZERO_IV[16]   = {0};
    if(n < 0x100 || d[0]!='N' || d[1]!='P' || d[2]!='D') return NULL;
    const uint8_t* digest   = d + 0x40;
    const uint8_t* dev_hash = d + 0x60;
    uint32_t flags     = sd_be32(d + 0x80);
    uint32_t block_size= sd_be32(d + 0x84);
    uint64_t file_size = sd_be64(d + 0x88);
    if(!(flags & 0x01000000u)) return NULL;   /* not SDATA (would need NPDRM license) */
    if(block_size == 0 || file_size == 0 || file_size > (uint64_t)n) return NULL;
    /* Only the plain (non-"compressed", non-0x20-metadata) layout is needed for
     * LBP's .sdat; bail (caller keeps ciphertext) on the exotic variants. */
    if((flags & 0x01/*COMPRESSED*/) || (flags & 0x20)) return NULL;
    int enc_key = (flags & 0x08) != 0;   /* EDAT_ENCRYPTED_KEY_FLAG */
    uint8_t crypt_key[16]; for(int i=0;i<16;i++) crypt_key[i]=dev_hash[i]^SDAT_KEY[i];
    uint64_t total_blocks = (file_size + block_size - 1) / block_size;
    uint8_t* out = (uint8_t*)malloc((size_t)file_size + 16);
    if(!out) return NULL;
    size_t written = 0;
    for(uint64_t bn=0; bn<total_blocks; bn++){
        uint64_t off = 0x100 + total_blocks*0x10 + bn*block_size;
        uint32_t length = block_size;
        if(bn==total_blocks-1 && (file_size % block_size)) length = (uint32_t)(file_size % block_size);
        uint32_t padlen = (length + 15) & ~15u;
        if(off + padlen > n){ free(out); return NULL; }
        uint8_t b_key[16]; memcpy(b_key, dev_hash, 12);
        b_key[12]=(uint8_t)(bn>>24); b_key[13]=(uint8_t)(bn>>16); b_key[14]=(uint8_t)(bn>>8); b_key[15]=(uint8_t)bn;
        uint8_t rk[176]; sd_key_expand(crypt_key, rk);
        uint8_t key_result[16]; sd_encrypt_block(rk, b_key, key_result);
        uint8_t key_final[16];
        if(enc_key) sd_cbc_decrypt(EDAT_KEY_0, ZERO_IV, key_result, key_final, 16);
        else        memcpy(key_final, key_result, 16);
        sd_cbc_decrypt(key_final, digest, d + off, out + written, padlen);
        written += length;
    }
    *out_size = (size_t)file_size;
    return out;
}

#endif /* SDATA_DECRYPT_H */

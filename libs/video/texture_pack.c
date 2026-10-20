#include "texture_pack.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define HEADER 104u
#define MAX_RAW_BODY (UINT64_C(2147483648) - HEADER)
#define MAX_COMPRESSED UINT64_C(2147483648)

static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static uint32_t be32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t *p)
{ return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }
static void putbe32(uint8_t *p, uint32_t x)
{ p[0] = (uint8_t)(x >> 24); p[1] = (uint8_t)(x >> 16); p[2] = (uint8_t)(x >> 8); p[3] = (uint8_t)x; }

static const uint32_t k[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static void sha_block(uint32_t s[8], const uint8_t block[64])
{
    uint32_t w[64], a,b,c,d,e,f,g,h;
    for (unsigned i=0;i<16;i++) w[i]=be32(block+4*i);
    for (unsigned i=16;i<64;i++) {
        uint32_t x=w[i-15], y=w[i-2];
        w[i]=(rotr(y,17)^rotr(y,19)^(y>>10))+w[i-7]
            +(rotr(x,7)^rotr(x,18)^(x>>3))+w[i-16];
    }
    a=s[0];b=s[1];c=s[2];d=s[3];e=s[4];f=s[5];g=s[6];h=s[7];
    for (unsigned i=0;i<64;i++) {
        uint32_t t1=h+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t t2=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s[0]+=a;s[1]+=b;s[2]+=c;s[3]+=d;s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;
}
void texture_pack_sha256(const void *data, size_t len, uint8_t digest[32])
{
    static const uint32_t init[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint32_t s[8]; uint8_t last[128]={0};
    const uint8_t *p=(const uint8_t *)data;
    size_t remain=len, full=len/64;
    memcpy(s,init,sizeof s);
    for (size_t i=0;i<full;i++) sha_block(s,p+i*64);
    p+=full*64;remain-=full*64;
    memcpy(last,p,remain);last[remain]=0x80;
    uint64_t bits=(uint64_t)len*8u;
    size_t pad=remain>=56?128:64;
    for (unsigned i=0;i<8;i++) last[pad-1-i]=(uint8_t)(bits>>(8*i));
    sha_block(s,last);if(pad==128)sha_block(s,last+64);
    for (unsigned i=0;i<8;i++)putbe32(digest+4*i,s[i]);
}
void texture_pack_free(texture_pack *pack)
{
    if (!pack) return;
    free(pack->file);
    memset(pack,0,sizeof *pack);
}
static int read_compressed(FILE *f, uint8_t **data, size_t *size)
{
    uint8_t *buffer=NULL; size_t used=0,capacity=0;
    for (;;) {
        size_t got;
        if (used==capacity) {
            if (used==MAX_COMPRESSED) {
                int tail=fgetc(f);
                if (tail!=EOF || ferror(f)) { free(buffer); return 0; }
                *data=buffer;*size=used;return 1;
            }
            size_t next=capacity ? capacity*2u : 65536u;
            uint8_t *grown;
            if (next>MAX_COMPRESSED) next=(size_t)MAX_COMPRESSED;
            if (next<=capacity) { free(buffer); return 0; }
            grown=(uint8_t *)realloc(buffer,next);
            if (!grown) { free(buffer); return 0; }
            buffer=grown;capacity=next;
        }
        got=fread(buffer+used,1,capacity-used,f);
        used+=got;
        if (got==0) {
            if (ferror(f) || used==0) { free(buffer); return 0; }
            *data=buffer;*size=used;return 1;
        }
    }
}
int texture_pack_load(const char *root, const void *base, size_t base_len,
                      uint32_t format, uint32_t sw, uint32_t sh, texture_pack *out)
{
    uint8_t hash[32];
    size_t expected;
    if (!out) return 0;
    memset(out,0,sizeof *out);
    if (!base || format<1 || format>3 || !sw || !sh || sw>4096 || sh>4096) return 0;
    expected=(size_t)((sw+3u)/4u)*((sh+3u)/4u)*(format==1?8u:16u);
    if (base_len!=expected) return 0;
    texture_pack_sha256(base,base_len,hash);
    return texture_pack_load_hashed(root,hash,format,sw,sh,out);
}
int texture_pack_load_hashed(const char *root, const uint8_t hash[32],
                             uint32_t format, uint32_t sw, uint32_t sh, texture_pack *out)
{
    static const char hex[]="0123456789abcdef";
    uint8_t hdr[HEADER]; char name[128], path[4096];
    size_t pos=0; uint64_t body=0; FILE *f=NULL;
    uint32_t w,h,levels,version; uint64_t bytes;
    uint8_t *compressed=NULL; size_t compressed_size=0;
    if (!out) return 0;
    memset(out,0,sizeof *out);
    if (!root || !*root || !hash || format<1 || format>3 || !sw || !sh || sw>4096 || sh>4096) return 0;
    if (strlen(root)>3900) return 0;
    for (unsigned i=0;i<32;i++){name[2*i]=hex[hash[i]>>4];name[2*i+1]=hex[hash[i]&15];}
    snprintf(name+64,sizeof name-64,"-f%u-%ux%u.frp",format,sw,sh);
    if (snprintf(path,sizeof path,"%s/%s",root,name)>=(int)sizeof path) return 0;
    f=fopen(path,"rb");if(!f)return 0;
    if(fread(hdr,1,HEADER,f)!=HEADER)goto fail;
    version=le32(hdr+4);
    if(memcmp(hdr,"FTX4",4) || (version!=1 && version!=2) || le32(hdr+8)!=format ||
       le32(hdr+12)!=sw || le32(hdr+16)!=sh || memcmp(hdr+32,hash,32))goto fail;
    w=le32(hdr+20);h=le32(hdr+24);levels=le32(hdr+28);
    if(!((w==sw*2u && h==sh*2u) || (w==sw*4u && h==sh*4u)) || w>16384 || h>16384 || !levels || levels>16)goto fail;
    for(uint32_t i=0;i<levels;i++){
        bytes=(uint64_t)w*h*4u;
        if(bytes>MAX_RAW_BODY-body || w>UINT32_MAX/4u)goto fail;
        out->level_width[i]=w;out->level_height[i]=h;out->row_bytes[i]=w*4u;
        body+=bytes;
        if(w==1 && h==1 && i+1<levels)goto fail;
        w=w>1?w/2:1;h=h>1?h/2:1;
    }
    if(le64(hdr+64)!=body || body>MAX_RAW_BODY)goto fail;
    out->file=(uint8_t *)malloc((size_t)body);if(!out->file)goto fail;
    if(version==1) {
        if(fread(out->file,1,(size_t)body,f)!=(size_t)body || fgetc(f)!=EOF || ferror(f))goto fail;
    } else {
        uLongf output_size=(uLongf)body;
        uLong input_size;
        int status;
        if (!read_compressed(f,&compressed,&compressed_size))goto fail;
        input_size=(uLong)compressed_size;
        status=uncompress2(out->file,&output_size,compressed,&input_size);
        if (status!=Z_OK || output_size!=body || input_size!=compressed_size)goto fail;
        free(compressed);compressed=NULL;
    }
    { uint8_t body_hash[32];
      texture_pack_sha256(out->file,(size_t)body,body_hash);
      if(memcmp(hdr+72,body_hash,32))goto fail;
    }
    for(uint32_t i=0;i<levels;i++){
        out->level[i]=out->file+pos;
        pos+=(size_t)out->row_bytes[i]*out->level_height[i];
    }
    out->width=le32(hdr+20);out->height=le32(hdr+24);out->levels=levels;
    fclose(f);return 1;
fail:
    free(compressed);fclose(f);texture_pack_free(out);return 0;
}

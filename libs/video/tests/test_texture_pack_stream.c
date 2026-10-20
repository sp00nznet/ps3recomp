#include "texture_pack_stream.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
#  define sleep_ms(n) Sleep(n)
#else
#  include <unistd.h>
#  define sleep_ms(n) usleep((n) * 1000)
#endif

/* FTX4 v1 fixtures in the layout of test_texture_pack.c: a one-block BC1
 * source (4x4) and its 4x replacement, one 16x16 RGBA level. */
static void le32(unsigned char *p, unsigned x)
{ p[0]=(unsigned char)x;p[1]=(unsigned char)(x>>8);p[2]=(unsigned char)(x>>16);p[3]=(unsigned char)(x>>24); }
static void le64(unsigned char *p, unsigned long long x)
{ le32(p,(unsigned)x);le32(p+4,(unsigned)(x>>32)); }
static void hex32(const unsigned char *p,char *s)
{ static const char h[]="0123456789abcdef";for(int i=0;i<32;i++){s[2*i]=h[p[i]>>4];s[2*i+1]=h[p[i]&15];}s[64]=0; }

#define BODY (16u * 16u * 4u)
static void write_pack(const char *dir, const unsigned char base[8], unsigned char fill, int corrupt)
{
    static unsigned char file[104 + BODY];
    unsigned char digest[32]; char name[65], path[4096];
    memset(file,0,sizeof file);memcpy(file,"FTX4",4);
    le32(file+4,1);le32(file+8,1);le32(file+12,4);le32(file+16,4);
    le32(file+20,16);le32(file+24,16);le32(file+28,1);
    texture_pack_sha256(base,8,digest);memcpy(file+32,digest,32);
    le64(file+64,BODY);
    memset(file+104,fill,BODY);
    texture_pack_sha256(file+104,BODY,file+72);
    if (corrupt) file[104] ^= 1;          /* body no longer matches its hash */
    hex32(digest,name);
    snprintf(path,sizeof path,"%s/%s-f1-4x4.frp",dir,name);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(file,1,sizeof file,f)==sizeof file);assert(fclose(f)==0);
}

static int wait_ready(uint32_t ticket, texture_pack *out)
{
    for (int i = 0; i < 2000; i++) {
        const int r = texture_pack_stream_poll(ticket, out);
        if (r != TEXTURE_PACK_PENDING) return r;
        sleep_ms(1);
    }
    return TEXTURE_PACK_PENDING;
}

int main(int argc, char **argv)
{
    assert(argc == 2);   /* an existing empty directory */
    const char *dir = argv[1];
    unsigned char good[8] = {1,2,3,4,5,6,7,8}, bad[8] = {9,9,9,9,9,9,9,9}, absent[8] = {7,7,7,7,7,7,7,7};
    write_pack(dir, good, 0x5A, 0);
    write_pack(dir, bad, 0x11, 1);
    texture_pack pack; uint32_t ticket;
    int checks = 0;

    /* no file: answered from the listing, nothing queued */
    assert(texture_pack_stream_request(dir, absent, 8, 1, 4, 4, &ticket, &pack) == TEXTURE_PACK_NONE);
    assert(ticket == 0); checks++;
    /* wrong base size or format: rejected before hashing */
    assert(texture_pack_stream_request(dir, good, 7, 1, 4, 4, &ticket, &pack) == TEXTURE_PACK_NONE); checks++;
    assert(texture_pack_stream_request(dir, good, 8, 4, 4, 4, &ticket, &pack) == TEXTURE_PACK_NONE); checks++;

    /* first use: pending, then ready from the worker */
    int r = texture_pack_stream_request(dir, good, 8, 1, 4, 4, &ticket, &pack);
    assert(ticket != 0); checks++;
    if (r == TEXTURE_PACK_PENDING) r = wait_ready(ticket, &pack);
    assert(r == TEXTURE_PACK_READY); checks++;
    assert(pack.width == 16 && pack.height == 16 && pack.levels == 1 && pack.row_bytes[0] == 64); checks++;
    for (unsigned i = 0; i < BODY; i++) assert(pack.level[0][i] == 0x5A);
    checks++;
    texture_pack_stream_release(ticket);

    /* second use: served from the cache at once, same ticket */
    uint32_t again = 0;
    assert(texture_pack_stream_request(dir, good, 8, 1, 4, 4, &again, &pack) == TEXTURE_PACK_READY); checks++;
    assert(again == ticket && pack.level[0][0] == 0x5A); checks++;
    texture_pack_stream_release(again);

    /* a file that fails validation never becomes a replacement */
    r = texture_pack_stream_request(dir, bad, 8, 1, 4, 4, &ticket, &pack);
    if (r == TEXTURE_PACK_PENDING) r = wait_ready(ticket, &pack);
    assert(r == TEXTURE_PACK_NONE); checks++;
    assert(texture_pack_stream_request(dir, bad, 8, 1, 4, 4, &ticket, &pack) == TEXTURE_PACK_NONE && !ticket); checks++;

    texture_pack_stream_stats st;
    texture_pack_stream_get_stats(&st);
    assert(st.loads == 1 && st.failures == 1 && st.absent == 1 && st.cached_bytes == BODY); checks++;

    /* shutdown frees everything; the stream starts again on demand */
    texture_pack_stream_shutdown();
    texture_pack_stream_get_stats(&st);
    assert(st.cached_bytes == 0); checks++;
    r = texture_pack_stream_request(dir, good, 8, 1, 4, 4, &ticket, &pack);
    if (r == TEXTURE_PACK_PENDING) r = wait_ready(ticket, &pack);
    assert(r == TEXTURE_PACK_READY && pack.level[0][BODY - 1] == 0x5A); checks++;
    texture_pack_stream_release(ticket);
    texture_pack_stream_shutdown();
    printf("test_texture_pack_stream: %d checks passed\n", checks);
    return 0;
}

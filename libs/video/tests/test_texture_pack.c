#include "texture_pack.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void le32(unsigned char *p, unsigned x)
{ p[0]=(unsigned char)x;p[1]=(unsigned char)(x>>8);p[2]=(unsigned char)(x>>16);p[3]=(unsigned char)(x>>24); }
static void le64(unsigned char *p, unsigned long long x)
{ le32(p,(unsigned)x);le32(p+4,(unsigned)(x>>32)); }
static void hex32(const unsigned char *p,char *s)
{ static const char h[]="0123456789abcdef";for(int i=0;i<32;i++){s[2*i]=h[p[i]>>4];s[2*i+1]=h[p[i]&15];}s[64]=0; }
static void fixture(unsigned char *file,const unsigned char *base)
{
    unsigned char digest[32];
    memset(file,0,168);memcpy(file,"FTX4",4);
    le32(file+4,1);le32(file+8,1);le32(file+12,1);le32(file+16,1);
    le32(file+20,4);le32(file+24,4);le32(file+28,1);
    texture_pack_sha256(base,8,digest);memcpy(file+32,digest,32);
    le64(file+64,64);
    for(int i=0;i<64;i++)file[104+i]=(unsigned char)i;
    texture_pack_sha256(file+104,64,file+72);
}
static void write_file(const char *path,const void *data,size_t n)
{ FILE *f=fopen(path,"wb");assert(f);assert(fwrite(data,1,n,f)==n);assert(fclose(f)==0); }
static size_t v2_fixture(unsigned char *output,const unsigned char *base)
{
    unsigned char raw[168];uLongf compressed=compressBound(64);
    fixture(raw,base);memcpy(output,raw,104);le32(output+4,2);
    assert(compress2(output+104,&compressed,raw+104,64,Z_BEST_COMPRESSION)==Z_OK);
    return 104+(size_t)compressed;
}
int main(int argc,char **argv)
{
    static const char *abc="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    unsigned char digest[32],base[8]={0},file[168],v2[256]; char hex[65],path[4096];
    size_t v2_size;
    texture_pack pack;
    assert(argc==2);
    texture_pack_sha256("abc",3,digest);hex32(digest,hex);assert(strcmp(hex,abc)==0);
    texture_pack_sha256("",0,digest);hex32(digest,hex);
    assert(strcmp(hex,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")==0);
    assert(!texture_pack_load(NULL,base,8,1,1,1,&pack));
    assert(!texture_pack_load(argv[1],base,7,1,1,1,&pack));
    assert(!texture_pack_load(argv[1],base,9,1,1,1,&pack));
    assert(!texture_pack_load(argv[1],base,8,2,1,1,&pack));
    texture_pack_sha256(base,8,digest);hex32(digest,hex);
    assert(snprintf(path,sizeof path,"%s/%s-f1-1x1.frp",argv[1],hex)<(int)sizeof path);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* missing */
    fixture(file,base);write_file(path,file,sizeof file);
    assert(texture_pack_load(argv[1],base,8,1,1,1,&pack));
    assert(pack.width==4 && pack.height==4 && pack.levels==1 && pack.row_bytes[0]==16);
    assert(pack.level[0][0]==0 && pack.level[0][63]==63);
    texture_pack_free(&pack);
    file[32]^=1;write_file(path,file,sizeof file);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* hash */
    fixture(file,base);le32(file+12,2);write_file(path,file,sizeof file);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* source dimensions */
    fixture(file,base);le32(file+8,2);write_file(path,file,sizeof file);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* format */
    fixture(file,base);write_file(path,file,sizeof file-1);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* short body */
    fixture(file,base);write_file(path,file,103);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* short header */
    fixture(file,base);le32(file+28,0);write_file(path,file,sizeof file);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* zero mip count */
    fixture(file,base);le32(file+28,17);write_file(path,file,sizeof file);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* excess mip count */
    fixture(file,base);le32(file+20,8193);write_file(path,file,sizeof file);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* output dimension cap */
    fixture(file,base);le64(file+64,65);write_file(path,file,sizeof file);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* body length mismatch */
    fixture(file,base);write_file(path,file,sizeof file);
    { FILE *extra=fopen(path,"ab");assert(extra);assert(fputc(1,extra)!=EOF);assert(fclose(extra)==0); }
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* trailing byte */
    fixture(file,base);file[104]^=1;write_file(path,file,sizeof file);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* body integrity */
    fixture(file,base);le32(file+20,2);le32(file+24,2);le64(file+64,16);
    texture_pack_sha256(file+104,16,file+72);write_file(path,file,120);
    assert(texture_pack_load(argv[1],base,8,1,1,1,&pack));
    assert(pack.width==2 && pack.height==2 && pack.row_bytes[0]==8);
    texture_pack_free(&pack);
    fixture(file,base);le32(file+20,3);write_file(path,file,sizeof file);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* unapproved scale */
    v2_size=v2_fixture(v2,base);write_file(path,v2,v2_size);
    assert(texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* v2 roundtrip */
    assert(pack.width==4 && pack.height==4 && pack.levels==1);
    for(int i=0;i<64;i++)assert(pack.level[0][i]==(unsigned char)i);
    texture_pack_free(&pack);
    v2[v2_size-1]^=1;write_file(path,v2,v2_size);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* corrupt stream */
    v2_size=v2_fixture(v2,base);write_file(path,v2,v2_size-1);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* truncated stream */
    v2_size=v2_fixture(v2,base);v2[v2_size]=0;write_file(path,v2,v2_size+1);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* trailing stream byte */
    v2_size=v2_fixture(v2,base);le64(v2+64,2147483648ULL);write_file(path,v2,v2_size);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* excessive raw body */
    v2_size=v2_fixture(v2,base);le32(v2+20,16385);write_file(path,v2,v2_size);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* excessive dimension */
    v2_size=v2_fixture(v2,base);v2[72]^=1;write_file(path,v2,v2_size);
    assert(!texture_pack_load(argv[1],base,8,1,1,1,&pack)); /* uncompressed hash */
    remove(path);puts("texture_pack CPU tests passed");return 0;
}

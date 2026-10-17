/* Differential regression for the compact decoder. Standalone build:
 * clang -O2 -Iinclude -Ilibs/video tests/test_vertex_decode_equivalence.c
 *   libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c -o decode-test.exe
 */
#include "../libs/video/rsx_vertex_compact.h"
#include "../libs/video/rsx_commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static unsigned long long checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#if defined(__clang__) || defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

/* Independent frozen scalar behavior: dispatch remains inside the loop,
 * half conversion uses bit fields, and comparisons below are bitwise. */
static NOINLINE int reference(u32 type, u32 size, const u8 *p, float out[4])
{
    if (!p || !out) return 0;
    out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
    for (u32 i = 0; i < size && i < 4; ++i) {
        switch (type) {
        case RSX_VTX_TYPE_FLOAT: {
            u32 bits = (u32)p[4*i] << 24 | (u32)p[4*i+1] << 16 |
                       (u32)p[4*i+2] << 8 | p[4*i+3];
            memcpy(out+i, &bits, 4); break;
        }
        case RSX_VTX_TYPE_HALF: {
            u32 h = (u32)p[2*i] << 8 | p[2*i+1];
            u32 e = (h >> 10) & 31, m = h & 1023;
            u32 bits = (h & 32768) << 16;
            if (e == 31) bits |= 0x7f800000u | (m << 13);
            else if (e != 0) bits |= ((e + 112) << 23) | (m << 13);
            memcpy(out+i, &bits, 4); break;
        }
        case RSX_VTX_TYPE_UNORM8: out[i] = p[i] / 255.0f; break;
        case RSX_VTX_TYPE_UINT8: out[i] = (float)p[i]; break;
        case RSX_VTX_TYPE_SNORM16: {
            s16 v = (s16)((p[2*i] << 8) | p[2*i+1]);
            out[i] = v / 32767.0f; break;
        }
        case RSX_VTX_TYPE_SINT16: {
            s16 v = (s16)((p[2*i] << 8) | p[2*i+1]);
            out[i] = (float)v; break;
        }
        case RSX_VTX_TYPE_CMP32: {
            u32 w = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
            s32 x = w & 2047, y = (w >> 11) & 2047, z = (w >> 22) & 1023;
            if (x & 1024) x -= 2048;
            if (y & 1024) y -= 2048;
            if (z & 512) z -= 1024;
            out[0] = (float)x / 1023.0f; out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f; out[3] = 1.0f; return 1;
        }
        default: return 0;
        }
    }
    return 1;
}

static void compare(u32 type, u32 size, const u8 *p)
{
    struct { u32 before; float out[4]; u32 after; } a, b;
    memset(&a, 0xa5, sizeof(a)); b = a;
    int ra = reference(type, size, p, a.out);
    int rb = rsx_vertex_decode_element(type, size, p, b.out);
    CHECK(ra == rb);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);
}
static void be32(u8 *p, u32 w)
{
    p[0] = w >> 24; p[1] = w >> 16; p[2] = w >> 8; p[3] = w;
}
static u32 random_word(u32 *state)
{
    *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5;
    return *state;
}
static unsigned bytes_for(u32 type, u32 size)
{
    unsigned n = size < 4 ? size : 4;
    if (!n) return 0;
    switch(type) {
    case RSX_VTX_TYPE_FLOAT: return n*4;
    case RSX_VTX_TYPE_HALF: case RSX_VTX_TYPE_SNORM16: case RSX_VTX_TYPE_SINT16: return n*2;
    case RSX_VTX_TYPE_UNORM8: case RSX_VTX_TYPE_UINT8: return n;
    case RSX_VTX_TYPE_CMP32: return 4;
    default: return 0;
    }
}
static void guard_pages(void)
{
#ifdef _WIN32
    SYSTEM_INFO info; GetSystemInfo(&info);
    SIZE_T page = info.dwPageSize;
    u8 *mem = VirtualAlloc(NULL, page*2, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    CHECK(mem != NULL);
    memset(mem, 0x81, page);
    DWORD previous;
    CHECK(VirtualProtect(mem+page, page, PAGE_NOACCESS, &previous));
    const u32 sizes[] = {0,1,2,3,4,5,0xffffffffu};
    for (u32 t=0; t<10; ++t) for(unsigned s=0;s<7;++s) {
        unsigned bytes = bytes_for(t,sizes[s]);
        /* The invalid/zero cases deliberately point at the protected page. */
        compare(t,sizes[s],mem+page-bytes);
    }
    CHECK(VirtualFree(mem,0,MEM_RELEASE));
#else
    puts("guard-page check unavailable on this platform");
#endif
}

typedef int (*decode_fn)(u32,u32,const u8*,float*);
static volatile decode_fn call_decode;
static volatile u32 sink;
static double bench(decode_fn fn, const u8 *data)
{
    float out[4]; u32 bits = 0, accumulator = 0; call_decode = fn;
    clock_t start = clock();
    for (unsigned i=0;i<30000000;++i) {
        u32 type = (i & 1) ? RSX_VTX_TYPE_FLOAT : RSX_VTX_TYPE_HALF;
        call_decode(type, (i%3)+2, data+(i&31), out);
        memcpy(&bits,out,4); accumulator ^= bits;
    }
    sink = accumulator;
    return (double)(clock()-start)/CLOCKS_PER_SEC;
}
int main(int argc, char **argv)
{
    u8 storage[80]; u8 *p = storage+1; u32 state=0x5b628fe1;
    memset(storage,0,sizeof(storage));
    const u32 sizes[] = {0,1,2,3,4,5,0xffffffffu};
    for (u32 v=0;v<65536;++v) {
        for (unsigned i=0;i<4;++i) { u32 h=(v+i*8191)&65535; p[2*i]=h>>8; p[2*i+1]=h; }
        for (unsigned s=0;s<7;++s) {
            compare(RSX_VTX_TYPE_HALF,sizes[s],p);
            compare(RSX_VTX_TYPE_SNORM16,sizes[s],p);
            compare(RSX_VTX_TYPE_SINT16,sizes[s],p);
        }
    }
    /* All exponent/sign combinations with zero, subnormal, boundary and
     * signalling/quiet NaN mantissas, plus deterministic arbitrary payloads. */
    const u32 mantissas[]={0,1,0x3fffff,0x400000,0x400001,0x7ffffe,0x7fffff};
    for (unsigned sign=0;sign<2;++sign) for(unsigned exp=0;exp<256;++exp)
        for(unsigned m=0;m<7;++m) {
            u32 w=(sign<<31)|(exp<<23)|mantissas[m];
            for(unsigned i=0;i<4;++i) be32(p+4*i,w);
            for(unsigned s=0;s<7;++s) compare(RSX_VTX_TYPE_FLOAT,sizes[s],p);
        }
    for (u32 v=0;v<256;++v) {
        for(unsigned j=0;j<4;++j) p[j]=(u8)(v+j*73);
        for(unsigned s=0;s<7;++s) {
            compare(RSX_VTX_TYPE_UNORM8,sizes[s],p);
            compare(RSX_VTX_TYPE_UINT8,sizes[s],p);
        }
    }
    for(unsigned i=0;i<100000;++i) {
        for(unsigned j=0;j<16;++j) p[j]=(u8)random_word(&state);
        for(u32 t=0;t<10;++t) for(unsigned s=0;s<7;++s) compare(t,sizes[s],p);
        compare(0xffffffffu,i%6,p);
    }
    for(u32 v=0;v<2048;++v) {
        be32(p,v|((2047-v)<<11)|((v&1023)<<22));
        for(unsigned s=0;s<7;++s) compare(RSX_VTX_TYPE_CMP32,sizes[s],p);
    }
    float out[4], saved[4]; memset(out,0x5a,sizeof(out)); memcpy(saved,out,sizeof(out));
    CHECK(!rsx_vertex_decode_element(RSX_VTX_TYPE_FLOAT,4,NULL,out));
    CHECK(!memcmp(out,saved,sizeof(out)));
    CHECK(!rsx_vertex_decode_element(RSX_VTX_TYPE_FLOAT,4,p,NULL));
    guard_pages();
    printf("PASS: %llu bitwise differential and boundary checks\n",checks);
    if(argc>1 && !strcmp(argv[1],"--bench")) {
        for(unsigned i=0;i<sizeof(storage);++i) storage[i]=(u8)random_word(&state);
        for(unsigned run=0;run<5;++run) {
            double old_time=bench(reference,storage), new_time=bench(rsx_vertex_decode_element,storage);
            printf("CPU-only mixed FLOAT/HALF run %u: scalar %.6fs optimized %.6fs ratio %.3f\n",run+1,old_time,new_time,new_time/old_time);
        }
    }
    return 0;
}

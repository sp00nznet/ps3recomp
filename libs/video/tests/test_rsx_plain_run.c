/* Authored streams: compare the entire register machine and callback-time
 * state, not a duplicate of the candidate's admission classifier. No GPU. */
#include "rsx_dispatch.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static rsx_dispatch scalar, batched, before;
static u32 events[2], hashes[2];
static u32 rng = 0x5eeda11u;
static u32 random_word(void) { rng ^= rng << 13; rng ^= rng >> 17; return rng ^= rng << 5; }
static void be_write(u8* p, u32 v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static u32 be_read(const u8* p) { return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3]; }
static void event(void* user, const rsx_dispatch* r)
{
    unsigned id = (unsigned)(size_t)user;
    events[id]++;
    for (u32 i=0; i<RSX_DSP_NUM_REGS; i++) hashes[id] = hashes[id]*33u + r->regs[i] + r->seen[i];
}
static void event_arg(void* u, const rsx_dispatch* r, u32 arg) { event(u,r); hashes[(size_t)u] ^= arg; }
static void event_range(void* u, const rsx_dispatch* r, u32 a, u32 b) { event_arg(u,r,a^b); }
static void event_inline(void* u, const rsx_dispatch* r, const u8* p, u32 n) { event_arg(u,r,n); if(n) hashes[(size_t)u] ^= p[n-1]; }
static void reset(void)
{
    rsx_dispatch_sink s = {0};
    s.clear=event_arg; s.begin=event_arg; s.end=event; s.draw_arrays=event_range;
    s.draw_index_array=event_range; s.inline_array=event_inline; s.flip=event_arg; s.immediate_vertex=event;
    rsx_dispatch_init(&scalar,&s); s.user=(void*)1; rsx_dispatch_init(&batched,&s);
    memset(events,0,sizeof events); memset(hashes,0,sizeof hashes);
}
static int candidate(u32 m,const u8* p,u32 n,int increment)
{
#ifdef RSX_DSP_PLAIN_RUN
    return rsx_dispatch_plain_run(&batched,m,p,n,increment);
#else
    (void)m; (void)p; (void)n; (void)increment; return 0;
#endif
}
static unsigned accepted, rejected;
static void packet(u32 m,const u8* p,u32 n,int inc,int mutate)
{
    before=batched;
    int taken=candidate(m,p,n,inc);
    if (!taken) {
        assert(memcmp(&before,&batched,sizeof before)==0);
        rejected++;
    } else accepted++;
    for(u32 i=0;i<n;i++) {
        rsx_dispatch_method(&scalar,m+(inc?4*i:0),be_read(p+4*i));
        if(!taken) rsx_dispatch_method(&batched,m+(inc?4*i:0),be_read(p+4*i));
    }
    if(mutate && taken) batched.regs[m>>2]^=1;
    assert(events[0]==events[1] && hashes[0]==hashes[1]);
    rsx_dispatch copy=batched; copy.sink.user=scalar.sink.user;
    assert(memcmp(&scalar,&copy,sizeof scalar)==0);
}
static void bench(int batch,int changing)
{
    u8 bytes[128*4];
    for(unsigned i=0;i<128;i++) be_write(bytes+4*i,random_word());
    for(unsigned repeat=0;repeat<9;repeat++) {
        reset();
        const clock_t t0=clock();
        for(unsigned j=0;j<400000;j++) {
            if(changing) be_write(bytes+(j&127)*4,j);
            if(batch && candidate(0x1a00,bytes,128,1)) continue;
            for(unsigned i=0;i<128;i++) rsx_dispatch_method(&batched,0x1a00+i*4,be_read(bytes+4*i));
        }
        const double seconds=(double)(clock()-t0)/CLOCKS_PER_SEC;
        printf("bench mode=%s stream=%s sample=%u cpu_seconds=%.6f words=51200000 packets=400000 seen=%u value=%u\n",
               batch?"candidate":"scalar",changing?"changing":"repeated",repeat,seconds,batched.seen[0x1a00/4],batched.regs[0x1a00/4]);
    }
}
int main(int argc,char** argv)
{
    if(argc>1 && !strcmp(argv[1],"bench")) { for(int change=0;change<2;change++) for(int batch=0;batch<2;batch++) bench(batch,change); return 0; }
    const int mutate=argc>1 && !strcmp(argv[1],"mutate");
    const int quick=argc>1 && !strcmp(argv[1],"quick"); /* bounded sanitizer smoke; normal run is exhaustive */
    u8 bytes[8192]; for(unsigned i=0;i<sizeof bytes/4;i++) be_write(bytes+i*4,random_word());
    reset();
    packet(0x1a00,bytes,128,1,mutate);
    assert(accepted==1 && "complete known-state packet must use the run path");
    /* Callbacks see all earlier state, and all uploads/immediate forms retain
     * their scalar auto-increment/folding/emission behavior. */
    const u32 effects[]={0x1808,0x1814,0x1824,0x1818,0x1500,0x1508,0x1884,0x1900,0x1940,0x1984,0xa84,0x1c0c,0x1e40,0xb80,0x1f00,0x1d94,0xe944,0x1808};
    for(unsigned i=0;i<sizeof effects/sizeof effects[0];i++) packet(effects[i],bytes,4,0,0);
    /* Every aligned method, with changing and non-incrementing payloads. */
    for(u32 m=0;m<0x10000;m+=quick?256:4) packet(m,bytes,1,1,0);
    for(unsigned i=0;i<(quick?64:3000);i++) {
        if(!(i%100)) reset();
        u32 m=(random_word()%0x800)*4, n=1+random_word()%32;
        for(u32 j=0;j<n;j++) be_write(bytes+j*4,random_word());
        packet(m,bytes,n,random_word()&1,0);
    }
    /* Boundaries across safe ranges and execution windows; repeated writes
     * retain seen[] overflow and consume the most recent source contents. */
    for(u32 m=0;m<0x2000;m+=quick?128:4) packet(m,bytes,8,1,0);
    scalar.seen[0x1a00/4]=batched.seen[0x1a00/4]=0xfffffffdu;
    packet(0x1a00,bytes,32,0,0);
    be_write(bytes,0xabcdef01); packet(0x1a00,bytes,32,0,0);
    before=batched;
    assert(!candidate(0x0224,bytes,2,0)); /* reserved surface gap is not an admitted method */
    assert(!candidate(0x0220,bytes,4,1)); /* crossing that gap remains wholly scalar */
    assert(!candidate(0x1a01,bytes,4,1));
    assert(!candidate(0x1a00,bytes,0xffffffffu,1));
    assert(!candidate(0x1a00,bytes,0,1));
    assert(memcmp(&before,&batched,sizeof before)==0);
    reset(); packet(0xa20,bytes,8,1,0);
    printf("PASS scalar/run whole-state and ordered callback equivalence: accepted=%u rejected=%u\n",accepted,rejected);
    return 0;
}

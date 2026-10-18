/* CPU-only regression for the real production pacer, no device/game needed. */
#include "../audio_pacer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { unsigned holes, blocks, bursts, underruns, rebases; uint64_t min_interval; } result;
static result simulate(int paced, uint64_t stall_start, uint64_t stall_duration)
{
    /* 48 kHz device, 10 ms engine quantum, 1056-frame shared buffer.
     * Guest has two ready blocks and takes 1 ms to finish each notification.
     * Slots include sequence numbers so ring wraps cannot conceal stale data. */
    unsigned padding = paced ? 512 : 0;
    uint64_t available_at[1024];
    for (unsigned i=0;i<1024;++i)available_at[i]=UINT64_MAX;
    available_at[0]=available_at[1]=0;
    audio_pacer p; audio_pacer_init(&p,0,1000000);
    result r={0}; r.min_interval=UINT64_MAX; uint64_t last=UINT64_MAX;
    for (uint64_t now=0;now<1000000;now+=100) {
        if(now && now%10000==0) {
            if(padding<480){++r.underruns;padding=0;}else padding-=480;
        }
        if(now>=stall_start && now<stall_start+stall_duration)continue;
        while(1056-padding>=256) {
            if(paced&&!audio_pacer_ready(&p,now))break;
            if(available_at[r.blocks]>now)++r.holes;
            if(last==now)++r.bursts;
            if(last!=UINT64_MAX && now-last<r.min_interval)r.min_interval=now-last;
            last=now;
            available_at[r.blocks+2]=now+1000;
            ++r.blocks;padding+=256;
            if(paced)audio_pacer_consumed(&p,now);
        }
    }
    r.rebases=(unsigned)p.rebases; return r;
}
int main(void)
{
    result old=simulate(0,0,0), normal=simulate(1,0,0);
    assert(old.bursts>0 && old.holes>0);
    assert(normal.bursts==0 && normal.holes==0 && normal.underruns==0);
    /* Engine padding can defer the final fractional block at this window's
     * boundary; throughput must stay within one block of 48000 frames. */
    assert(normal.blocks>=187 && normal.blocks<=188);
    assert(normal.min_interval>=2666 && normal.rebases==0);
    result short_stall=simulate(1,100000,12000);
    assert(short_stall.bursts==0 && short_stall.holes==0);
    assert(short_stall.blocks>=187 && short_stall.blocks<=188);
    assert(short_stall.min_interval>=2666 && short_stall.rebases==0);
    result suspend=simulate(1,100000,100000);
    assert(suspend.bursts==0 && suspend.holes==0 && suspend.underruns>0);
    assert(suspend.rebases==1 && suspend.min_interval>=2666);
    audio_pacer p;audio_pacer_init(&p,0,10000000);
    for(unsigned i=0;i<187500;i++) {
        uint64_t due=audio_pacer_offset(&p);
        if(i)assert(!audio_pacer_ready(&p,due-1));
        assert(audio_pacer_ready(&p,due));audio_pacer_consumed(&p,due);
    }
    assert(audio_pacer_offset(&p)==10000000000ULL); /* 1000 s exactly */
    printf("PASS: old holes=%u bursts=%u; paced holes=%u bursts=%u underruns=%u; short stall holes=%u; suspension visible underruns=%u; rational 1000s exact\n",
        old.holes,old.bursts,normal.holes,normal.bursts,normal.underruns,short_stall.holes,suspend.underruns);
    return 0;
}

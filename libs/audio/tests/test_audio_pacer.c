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
/* Device-clock model for the standing fill. The endpoint runs on its own
 * crystal (drift_ppm against the counter) and pulls one 480-frame period per
 * pass, but every 23rd pass is late and pulls two at once (as traced on the
 * real endpoint: the queue fell from ~860 frames to 0 in one pass). The
 * producer wakes up to wake_jitter us late. Counts passes that found too few
 * frames (the engine plays zeros: an underrun) and frames the device had no
 * room for. */
typedef struct { unsigned underruns, bursts; unsigned long long produced, consumed, dropped;
                 uint64_t min_interval; uint32_t min_fill, max_fill, target; double late_avg_fill; } fill_result;
static fill_result simulate_fill(int steered, double drift_ppm, uint64_t wake_jitter, uint64_t seconds)
{
    const uint64_t f = 1000000;
    const uint32_t buffer = steered ? 4800 : 1056, period = 480;
    const uint32_t target = audio_fill_target(period, buffer);
    uint32_t padding = steered ? target : 512;
    audio_pacer p; audio_pacer_init(&p, 0, f);
    audio_fill fill = {0}; fill.target = target;
    fill_result r = {0}; r.min_interval = UINT64_MAX; r.min_fill = UINT32_MAX;
    uint64_t last = UINT64_MAX, passes = 0, next_wake = 0;
    double device_clock = 0;
    unsigned rng = 12345; double late_sum = 0; uint64_t late_n = 0;
    r.target = target;
    for (uint64_t now = 0; now < seconds * f; now += 50) {
        device_clock += 50.0 * (1.0 + drift_ppm * 1e-6);
        while (device_clock >= (passes + 1) * 10000.0) {
            ++passes;
            if (passes % 23 == 22) continue;                 /* late pass ... */
            uint32_t want = passes % 23 == 0 ? period * 2 : period; /* ... pulls two */
            if (padding < want) { ++r.underruns; r.consumed += padding; padding = 0; }
            else { padding -= want; r.consumed += want; }
        }
        if (now > 2 * f && padding < r.min_fill) r.min_fill = padding;
        if (now > 2 * f && padding > r.max_fill) r.max_fill = padding;
        if (now > (seconds - 60) * f) { late_sum += padding; ++late_n; }
        if (now < next_wake) continue;
        if (buffer - padding < 256) continue;
        if (!audio_pacer_ready(&p, now)) {
            rng = rng * 1103515245u + 12345u;
            uint64_t due = audio_pacer_deadline(&p);
            next_wake = due + (wake_jitter ? (rng >> 8) % wake_jitter : 0);
            continue;
        }
        if (steered) audio_fill_observe(&fill, &p, padding);
        if (last == now) ++r.bursts;
        if (last != UINT64_MAX && now - last < r.min_interval) r.min_interval = now - last;
        last = now;
        audio_pacer_consumed(&p, now);
        padding += 256; r.produced += 256;
    }
    r.late_avg_fill = late_n ? late_sum / late_n : 0;
    return r;
}

int main(void)
{
    /* The old two-block prime runs dry on a double pull even without drift. */
    fill_result old_fill = simulate_fill(0, 0.0, 1500, 60);
    assert(old_fill.underruns > 0);
    /* Standing fill + trim: no underrun with jittery wakes and double pulls,
     * at zero drift and with the device 300 ppm fast or slow, for 20 minutes. */
    const double drifts[] = {0.0, 300.0, -300.0};
    for (unsigned i = 0; i < 3; ++i) {
        fill_result r = simulate_fill(1, drifts[i], 1500, 1200);
        assert(r.underruns == 0 && r.bursts == 0 && r.dropped == 0);
        assert(r.min_interval >= 2666);
        /* Production follows the device: after 20 minutes the average fill
         * is still at the target, so nothing accumulates or drains. */
        assert(r.late_avg_fill > r.target - 256 && r.late_avg_fill < r.target + 256);
        assert(r.min_fill > 0 && r.max_fill < 4800 - 256);
        printf("fill drift %+.0f ppm: min %u max %u, last-minute average %.0f (target %u) frames\n",
               drifts[i], r.min_fill, r.max_fill, r.late_avg_fill, r.target);
    }
    /* Untrimmed, the same 300 ppm drift drains the fill: the trim is needed. */
    {
        audio_pacer p; audio_pacer_init(&p, 0, 1000000);
        double device = 0; long long fill = 1472;
        for (uint64_t now = 0; now < 1200ull * 1000000; now += 100) {
            device += 100.0 * 1.0003;
            while (device >= 480.0 / 48000.0 * 1e6) { device -= 480.0 / 48000.0 * 1e6; fill -= 480; }
            if (audio_pacer_ready(&p, now)) { audio_pacer_consumed(&p, now); fill += 256; }
        }
        assert(fill < 0); /* ran dry within 20 minutes */
    }
    /* audio_fill_target: two periods and two blocks, bounded by the buffer. */
    assert(audio_fill_target(480, 4800) == 1472);
    assert(audio_fill_target(480, 1056) == 544);
    printf("PASS fill: old prime underruns=%u in 60 s; steered 0 underruns at 0/+300/-300 ppm for 1200 s\n",
           old_fill.underruns);

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

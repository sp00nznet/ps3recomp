/* rsx_present_guard.h: a window nobody can see never gates the presenter.
 * python3 tools/test_present_guard.py */
#include "../rsx_present_guard.h"
#include <stdio.h>
static int fails;
static void check(int ok, const char* what) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", what); fails += !ok; }
#define MS 1000000ull
int main(void)
{
    rsx_present_guard g; uint64_t t = 0;
    /* Default bound; a healthy swapchain keeps the bounded wait. */
    rsx_present_guard_init(&g, NULL);
    check(g.bound_ns == 100 * MS, "the acquire bound defaults to 100 ms");
    check(rsx_present_guard_before(&g, 0, &t) && t == 100 * MS, "a visible window acquires with the bound");
    rsx_present_guard_acquired(&g, 1);
    rsx_present_guard_presented(&g, 1 * MS, 2 * MS);
    check(rsx_present_guard_before(&g, 17 * MS, &t) && t == 100 * MS && !g.skipped, "and keeps it while frames go out");

    /* Hidden: skip without touching the swapchain; back when visible. */
    rsx_present_guard_set_hidden(&g, 1);
    check(!rsx_present_guard_before(&g, 20 * MS, &t) && g.skipped == 1, "a hidden window or a sleeping display is skipped");
    rsx_present_guard_set_hidden(&g, 0);
    check(rsx_present_guard_before(&g, 40 * MS, &t) && t == 100 * MS, "and shown again once visible");

    /* An acquire that times out: later acquires do not wait, until one gets an image. */
    rsx_present_guard_acquired(&g, 0);
    check(g.acquire_stalled && g.stalls == 1 && g.skipped == 2, "a timed-out acquire marks the swapchain stalled");
    check(rsx_present_guard_before(&g, 150 * MS, &t) && t == 0, "a stalled swapchain is polled without waiting");
    rsx_present_guard_acquired(&g, 0);
    check(g.stalls == 1, "one stall is counted once");
    check(rsx_present_guard_before(&g, 160 * MS, &t) && t == 0, "still without waiting");
    rsx_present_guard_acquired(&g, 1);
    check(rsx_present_guard_before(&g, 170 * MS, &t) && t == 100 * MS, "an image ends the stall");

    /* A present that overruns the bound: skip, probe every 2 s, recover on a quick one. */
    const uint64_t at = 1000 * MS;
    rsx_present_guard_presented(&g, at, 250 * MS);
    check(g.present_stalled && g.stalls == 2, "a present over the bound marks presentation stalled");
    const unsigned long long before = g.skipped;
    int shown = 0;
    for (uint64_t now = at; now < at + 1990 * MS; now += 16 * MS) shown += rsx_present_guard_before(&g, now, &t);
    check(!shown && g.skipped > before + 100, "the window is skipped between probes");
    check(rsx_present_guard_before(&g, at + 2000 * MS, &t), "a probe after 2 s");
    rsx_present_guard_acquired(&g, 1);
    rsx_present_guard_presented(&g, at + 2250 * MS, 250 * MS);
    check(!rsx_present_guard_before(&g, at + 2260 * MS, &t), "a slow probe keeps it stalled");
    check(rsx_present_guard_before(&g, at + 4300 * MS, &t), "the next probe");
    rsx_present_guard_presented(&g, at + 4302 * MS, 2 * MS);
    check(rsx_present_guard_before(&g, at + 4310 * MS, &t) && t == 100 * MS && !g.present_stalled,
          "a quick probe ends the stall");

    /* A quick present call alone does not end a stall; a whole frame does. */
    rsx_present_guard_waited(&g, at + 5000 * MS, 300 * MS);
    check(g.present_stalled && g.stalls == 3, "a long wait on the window marks it stalled");
    rsx_present_guard_waited(&g, at + 5001 * MS, 1 * MS);
    check(g.present_stalled, "a quick present call does not end it");
    rsx_present_guard_presented(&g, at + 7100 * MS, 3 * MS);
    check(!g.present_stalled, "a quick whole frame does");

    /* A presenter that cannot poll (Metal): nextDrawable outcomes. */
    rsx_present_guard_init(&g, NULL);
    rsx_present_guard_drawable(&g, 10 * MS, 5 * MS, 1);
    check(!g.present_stalled && rsx_present_guard_before(&g, 11 * MS, &t), "a drawable within the bound is a frame through");
    rsx_present_guard_drawable(&g, 1100 * MS, 1000 * MS, 0);
    check(g.present_stalled && !rsx_present_guard_before(&g, 1200 * MS, &t), "nil after a second's wait skips the window");
    check(rsx_present_guard_before(&g, 3100 * MS, &t), "with a probe after 2 s");
    rsx_present_guard_drawable(&g, 3105 * MS, 4 * MS, 1);
    check(!g.present_stalled, "a quick drawable on the probe ends it");
    rsx_present_guard_drawable(&g, 3200 * MS, 1 * MS, 0);
    check(!g.present_stalled, "a quick nil (compositor busy once) is not a stall");
    check(rsx_present_guard_slot_wait(&g, 150 * MS) && !rsx_present_guard_slot_wait(&g, 20 * MS) && !g.present_stalled,
          "a long frame-slot wait is reported but never skips the window");

    /* Frames left out of the window are held to 60 Hz. */
    rsx_present_guard_init(&g, NULL);
    const uint64_t P = RSX_PRESENT_GUARD_PACE_NS;
    check(rsx_present_guard_pace_ns(&g, 0) == 0, "the first skipped frame goes at once");
    check(rsx_present_guard_pace_ns(&g, 1 * MS) == P - 1 * MS, "the next waits for its 60 Hz slot");
    uint64_t now = P;                               /* it slept until its slot */
    int frames = 1; uint64_t slept = 0;
    for (int i = 0; i < 59; i++) { const uint64_t w = rsx_present_guard_pace_ns(&g, now); slept += w; now += w; frames++; }
    check(frames == 60 && now >= 59 * P && now <= 60 * P, "a spinning presenter is held to 60 frames a second");
    const uint64_t late = now + 5 * P;              /* a slow frame */
    check(rsx_present_guard_pace_ns(&g, late) == 0 && rsx_present_guard_pace_ns(&g, late + 1 * MS) == P - 1 * MS,
          "a presenter behind by more than a period starts a new grid, no catch-up burst");
    (void)slept;

    /* Off: the old unbounded acquire, nothing skipped. */
    rsx_present_guard_init(&g, "0");
    rsx_present_guard_set_hidden(&g, 1);
    rsx_present_guard_acquired(&g, 0);
    rsx_present_guard_presented(&g, 0, 900 * MS);
    check(rsx_present_guard_before(&g, 0, &t) && t == UINT64_MAX && !g.skipped,
          "PS3RECOMP_VK_ACQUIRE_BOUND_MS=0 keeps the unbounded acquire");
    rsx_present_guard_init(&g, "40");
    check(g.bound_ns == 40 * MS, "a bound can be set");
    rsx_present_guard_init(&g, "junk");
    check(g.bound_ns == 100 * MS, "a malformed bound keeps the default");

    printf("[present_guard] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

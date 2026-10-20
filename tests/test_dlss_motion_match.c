#include "../libs/video/rsx_dlss_motion_match.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint32_t random_state = UINT32_C(0x91e10da5);

static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static int reference_equal(const rsx_dlss_draw_key *a,
                           const rsx_dlss_draw_key *b, int count_matters)
{
    return a->geometry == b->geometry && a->slots == b->slots &&
           a->topology == b->topology && (!count_matters || a->count == b->count);
}

static void reference_match(const rsx_dlss_draw_key *previous, uint32_t previous_count,
                            const rsx_dlss_draw_key *current, uint32_t current_count,
                            int reset, uint32_t (*map)[4])
{
    uint32_t i, j;
    for (i = 0; i < current_count; ++i) {
        uint32_t previous_matches = 0, current_matches = 0, matched = 0;
        map[i][0] = 0;
        map[i][1] = current[i].base;
        map[i][2] = 0;
        map[i][3] = 0;
        if (reset || !current[i].geometry || !current[i].slots)
            continue;
        for (j = 0; j < previous_count; ++j) {
            if (reference_equal(&current[i], &previous[j], 1)) {
                ++previous_matches;
                matched = j;
            }
        }
        for (j = 0; j < current_count; ++j)
            if (reference_equal(&current[i], &current[j], 0))
                ++current_matches;
        if (previous_matches == 1 && current_matches == 1) {
            map[i][0] = previous[matched].base;
            map[i][2] = 1;
        }
    }
}

static void compare(const rsx_dlss_draw_key *previous, uint32_t previous_count,
                    const rsx_dlss_draw_key *current, uint32_t current_count, int reset)
{
    uint32_t actual[256][4], expected[256][4];
    memset(actual, 0xa5, sizeof(actual));
    memset(expected, 0x5a, sizeof(expected));
    assert(rsx_dlss_match_draws(previous, previous_count, current, current_count,
                                reset, actual) == 1);
    reference_match(previous, previous_count, current, current_count, reset, expected);
    assert(memcmp(actual, expected, current_count * sizeof(actual[0])) == 0);
}

static rsx_dlss_draw_key make_key(uint64_t geometry, uint64_t slots,
                                  uint64_t topology, uint32_t base, uint32_t count)
{
    rsx_dlss_draw_key key;
    key.geometry = geometry;
    key.slots = slots;
    key.topology = topology;
    key.base = base;
    key.count = count;
    return key;
}

static void fixed_cases(void)
{
    rsx_dlss_draw_key p[4], c[4];
    uint32_t result[4][4];
    p[0] = make_key(UINT64_C(0x100000001), UINT64_C(0x200000001), 7, 100, 3);
    p[1] = make_key(12, 22, 8, 200, 4);
    c[0] = p[1]; c[0].base = 900;
    c[1] = p[0]; c[1].base = 800;
    compare(p, 2, c, 2, 0);
    assert(rsx_dlss_match_draws(p, 2, c, 2, 0, result) == 1);
    assert(result[0][0] == 200 && result[0][1] == 900 && result[0][2] == 1);
    assert(result[1][0] == 100 && result[1][1] == 800 && result[1][2] == 1);
    c[0].geometry++; compare(p, 2, c, 2, 0); c[0].geometry--;
    c[0].slots++; compare(p, 2, c, 2, 0); c[0].slots--;
    c[0].topology++; compare(p, 2, c, 2, 0); c[0].topology--;
    c[0].count++; compare(p, 2, c, 2, 0); c[0].count--;
    c[0].geometry = 0; compare(p, 2, c, 2, 0); c[0] = p[1];
    c[0].slots = 0; compare(p, 2, c, 2, 0); c[0] = p[1];
    p[2] = p[0]; p[2].base = 333; compare(p, 3, c, 2, 0);
    p[2].count++; compare(p, 3, c, 2, 0);
    c[2] = c[0]; c[2].count++; c[2].base = 999;
    compare(p, 2, c, 3, 0);
    assert(rsx_dlss_match_draws(p, 2, c, 3, 0, result) == 1);
    assert(result[0][2] == 0 && result[2][2] == 0);
    compare(p, 2, c, 3, 1);
    compare(0, 0, c, 3, 0);
    compare(p, 2, 0, 0, 0);
    compare(0, 0, 0, 0, 1);
    p[2] = make_key(UINT64_C(0x100000000), 1, 1, 333, 1);
    p[3] = make_key(0, 1, 1, 444, 1);
    c[0] = p[2]; c[1] = p[3];
    compare(p, 4, c, 2, 0);
    assert(rsx_dlss_match_draws(p, 257, c, 2, 0, result) == 0);
    assert(rsx_dlss_match_draws(p, 2, c, 257, 0, result) == 0);
    assert(rsx_dlss_match_draws(0, 1, c, 2, 0, result) == 0);
    assert(rsx_dlss_match_draws(p, 2, 0, 1, 0, result) == 0);
    assert(rsx_dlss_match_draws(p, 2, c, 1, 0, 0) == 0);
}

static void collision_case(void)
{
    rsx_dlss_draw_key p[3], c[3];
    uint64_t candidate;
    uint32_t bucket;
    p[0] = make_key(1, 2, 3, 10, 4);
    bucket = rsx_dlss_match_hash(&p[0], 1);
    for (candidate = 2; candidate < 100000; ++candidate) {
        p[1] = make_key(candidate, 2, 3, 20, 4);
        if (rsx_dlss_match_hash(&p[1], 1) == bucket)
            break;
    }
    assert(candidate < 100000);
    p[2] = make_key(9, 4, 5, 30, 2);
    c[0] = p[1]; c[0].base = 40;
    c[1] = p[0]; c[1].base = 50;
    c[2] = p[2]; c[2].base = 60;
    compare(p, 3, c, 3, 0);
}

static void randomized_cases(void)
{
    rsx_dlss_draw_key p[256], c[256];
    uint32_t iteration, i, pn, cn;
    for (iteration = 0; iteration < 1200; ++iteration) {
        pn = iteration == 0 ? 256u : next_random() % 129u;
        cn = iteration == 0 ? 256u : next_random() % 129u;
        for (i = 0; i < pn; ++i) {
            p[i] = make_key((uint64_t)(next_random() % 38u),
                            (uint64_t)(next_random() % 34u),
                            (uint64_t)(next_random() % 13u), i * 7u,
                            next_random() % 9u);
            if (i && (next_random() & 7u) == 0)
                p[i] = p[next_random() % i];
            if ((next_random() & 31u) == 0)
                p[i].geometry |= UINT64_C(1) << 40;
        }
        for (i = 0; i < cn; ++i) {
            if (pn && (next_random() & 1u))
                c[i] = p[next_random() % pn];
            else
                c[i] = make_key((uint64_t)(next_random() % 38u),
                                (uint64_t)(next_random() % 34u),
                                (uint64_t)(next_random() % 13u), i,
                                next_random() % 9u);
            if (i && (next_random() & 15u) == 0)
                c[i] = c[next_random() % i];
            if ((next_random() & 31u) == 0)
                c[i].count++;
            c[i].base = i * 11u + 1000u;
        }
        compare(p, pn, c, cn, (iteration % 17u) == 0);
    }
}

static void benchmark(void)
{
    rsx_dlss_draw_key p[256], c[256];
    uint32_t map[256][4], i, run;
    clock_t start, fast_ticks, reference_ticks;
    volatile uint32_t sink = 0;
    for (i = 0; i < 256u; ++i) {
        p[i] = make_key(UINT64_C(0x100000000) + i, 100 + i,
                        200 + i, i * 3u, i + 1u);
    }
    for (i = 0; i < 256u; ++i) {
        c[i] = p[255u - i];
        c[i].base = i * 5u;
    }
    start = clock();
    for (run = 0; run < 1000u; ++run) {
        assert(rsx_dlss_match_draws(p, 256, c, 256, 0, map) == 1);
        sink += map[run & 255u][0];
    }
    fast_ticks = clock() - start;
    start = clock();
    for (run = 0; run < 1000u; ++run) {
        reference_match(p, 256, c, 256, 0, map);
        sink += map[run & 255u][0];
    }
    reference_ticks = clock() - start;
    printf("256-key CPU match: table %.3f ms, reference %.3f ms per 1000 calls (sink=%u)\n",
           1000.0 * (double)fast_ticks / (double)CLOCKS_PER_SEC,
           1000.0 * (double)reference_ticks / (double)CLOCKS_PER_SEC, (unsigned)sink);
}

int main(void)
{
    fixed_cases();
    collision_case();
    randomized_cases();
    benchmark();
    puts("motion matching: fixed, collision, 1200 randomized cases passed");
    return 0;
}

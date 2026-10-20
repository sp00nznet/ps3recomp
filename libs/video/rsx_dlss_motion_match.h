#ifndef RSX_DLSS_MOTION_MATCH_H
#define RSX_DLSS_MOTION_MATCH_H

#include <stdint.h>
#include <string.h>

#define RSX_DLSS_MATCH_TABLE_SIZE 512u
#define RSX_DLSS_MATCH_TABLE_MASK (RSX_DLSS_MATCH_TABLE_SIZE - 1u)

typedef struct rsx_dlss_draw_key {
    uint64_t geometry;
    uint64_t slots;
    uint64_t topology;
    uint32_t base;
    uint32_t count;
} rsx_dlss_draw_key;

/* The base identifies a draw's location, but is intentionally not part of its identity. */
typedef struct rsx_dlss_match_entry {
    rsx_dlss_draw_key key;
    uint32_t index;
    uint32_t occurrences;
    uint8_t occupied;
} rsx_dlss_match_entry;

static inline uint64_t rsx_dlss_mix64(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static inline uint32_t rsx_dlss_match_hash(const rsx_dlss_draw_key *key, int include_count)
{
    uint64_t value = rsx_dlss_mix64(key->geometry);
    value ^= rsx_dlss_mix64(key->slots + UINT64_C(0x9e3779b97f4a7c15));
    value ^= rsx_dlss_mix64(key->topology + UINT64_C(0xd1b54a32d192ed03));
    if (include_count)
        value ^= rsx_dlss_mix64((uint64_t)key->count + UINT64_C(0x94d049bb133111eb));
    return (uint32_t)rsx_dlss_mix64(value) & RSX_DLSS_MATCH_TABLE_MASK;
}

static inline int rsx_dlss_match_equal(const rsx_dlss_draw_key *a,
                                       const rsx_dlss_draw_key *b, int include_count)
{
    return a->geometry == b->geometry && a->slots == b->slots &&
           a->topology == b->topology && (!include_count || a->count == b->count);
}

/* A spare-capacity table permits at most 256 occupied entries; lookup always terminates. */
static inline rsx_dlss_match_entry *rsx_dlss_match_find(
    rsx_dlss_match_entry table[RSX_DLSS_MATCH_TABLE_SIZE],
    const rsx_dlss_draw_key *key, int include_count)
{
    uint32_t slot = rsx_dlss_match_hash(key, include_count);
    uint32_t probe;
    for (probe = 0; probe < RSX_DLSS_MATCH_TABLE_SIZE; ++probe) {
        rsx_dlss_match_entry *entry = &table[slot];
        if (!entry->occupied || rsx_dlss_match_equal(&entry->key, key, include_count))
            return entry;
        slot = (slot + 1u) & RSX_DLSS_MATCH_TABLE_MASK;
    }
    return 0;
}

/* map rows: previousBase, currentBase, valid, reserved. */
static inline int rsx_dlss_match_draws(const rsx_dlss_draw_key *previous,
                                       uint32_t previous_count,
                                       const rsx_dlss_draw_key *current,
                                       uint32_t current_count, int reset,
                                       uint32_t (*map)[4])
{
    rsx_dlss_match_entry previous_table[RSX_DLSS_MATCH_TABLE_SIZE];
    rsx_dlss_match_entry current_table[RSX_DLSS_MATCH_TABLE_SIZE];
    uint32_t i;
    if (previous_count > 256u || current_count > 256u ||
        (previous_count && !previous) || (current_count && (!current || !map)))
        return 0;

    for (i = 0; i < current_count; ++i) {
        map[i][0] = 0;
        map[i][1] = current[i].base;
        map[i][2] = 0;
        map[i][3] = 0;
    }
    if (reset || !previous_count || !current_count)
        return 1;

    memset(previous_table, 0, sizeof(previous_table));
    memset(current_table, 0, sizeof(current_table));

    for (i = 0; i < previous_count; ++i) {
        rsx_dlss_match_entry *entry;
        if (!previous[i].geometry || !previous[i].slots)
            continue;
        entry = rsx_dlss_match_find(previous_table, &previous[i], 1);
        if (!entry)
            return 0;
        if (entry->occupied) {
            ++entry->occurrences;
        } else {
            entry->key = previous[i];
            entry->index = i;
            entry->occurrences = 1;
            entry->occupied = 1;
        }
    }
    for (i = 0; i < current_count; ++i) {
        rsx_dlss_match_entry *entry;
        if (!current[i].geometry || !current[i].slots)
            continue;
        entry = rsx_dlss_match_find(current_table, &current[i], 0);
        if (!entry)
            return 0;
        if (entry->occupied) {
            ++entry->occurrences;
        } else {
            entry->key = current[i];
            entry->index = i;
            entry->occurrences = 1;
            entry->occupied = 1;
        }
    }
    for (i = 0; i < current_count; ++i) {
        rsx_dlss_match_entry *current_entry;
        rsx_dlss_match_entry *previous_entry;
        if (!current[i].geometry || !current[i].slots)
            continue;
        current_entry = rsx_dlss_match_find(current_table, &current[i], 0);
        if (!current_entry || current_entry->occurrences != 1u)
            continue;
        previous_entry = rsx_dlss_match_find(previous_table, &current[i], 1);
        if (previous_entry && previous_entry->occupied && previous_entry->occurrences == 1u) {
            map[i][0] = previous[previous_entry->index].base;
            map[i][2] = 1;
        }
    }
    return 1;
}

#endif

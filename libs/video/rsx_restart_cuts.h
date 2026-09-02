#ifndef PS3RECOMP_RSX_RESTART_CUTS_H
#define PS3RECOMP_RSX_RESTART_CUTS_H

#include <stdlib.h>

/* A draw may contain one primitive-restart marker for every tiny strip.
 * Keep the cut list growable instead of imposing an arbitrary per-draw cap. */
static inline int rsx_restart_cut_push(u32** cuts, u32* count, u32* capacity,
                                       u32 vertex_offset)
{
    if (*count == *capacity) {
        u32 next = *capacity ? *capacity * 2u : 1024u;
        u32* grown = (u32*)realloc(*cuts, (size_t)next * sizeof(*grown));
        if (!grown)
            return 0;
        *cuts = grown;
        *capacity = next;
    }
    (*cuts)[(*count)++] = vertex_offset;
    return 1;
}

static inline void rsx_restart_segment_bounds(const u32* cuts, u32 cut_count,
                                               u32 vertex_count, u32 segment,
                                               u32* first, u32* count)
{
    const u32 begin = segment ? cuts[segment - 1] : 0;
    const u32 end = segment < cut_count ? cuts[segment] : vertex_count;
    *first = begin;
    *count = end > begin ? end - begin : 0;
}

#endif

/*
 * ps3recomp - RSX primitive types
 *
 * Two questions every backend has to answer about a primitive, and neither is
 * host-API business:
 *
 *   - can the hardware draw it directly, or must the CPU expand it first?
 *   - which of the topologies every modern API does have does it become?
 *
 * Both were being answered separately in three places, and the three did not
 * agree. The D3D12 backend dispatched on bare numbers (`primitive == 8`) with
 * the name only in a comment. The Metal backend had its own prim_to_metal and
 * prim_needs_expansion. This header had a third set that nothing called at all
 * -- and which counted LINE_LOOP as needing conversion while leaving POLYGON
 * out, the opposite of what Metal did. Those dead helpers generated index
 * buffers, which is not how either backend actually works: both expand
 * vertices on the CPU. They are gone rather than reconciled; a third backend
 * following them would have written the wrong thing.
 */
#ifndef PS3RECOMP_RSX_PRIMITIVES_H
#define PS3RECOMP_RSX_PRIMITIVES_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The topologies every modern host API has. A backend maps these to its own
 * enum; nothing here assumes any particular API's numbering. */
typedef enum {
    RSX_TOPOLOGY_UNSUPPORTED = 0,
    RSX_TOPOLOGY_POINTS,
    RSX_TOPOLOGY_LINES,
    RSX_TOPOLOGY_LINE_STRIP,
    RSX_TOPOLOGY_TRIANGLES,
    RSX_TOPOLOGY_TRIANGLE_STRIP
} rsx_topology;

/* Does drawing this primitive require the CPU to expand it into triangles
 * first?
 *
 * Quads, quad strips, triangle fans and polygons have no equivalent on any
 * modern API, so their vertices must be re-emitted as a triangle list.
 *
 * LINE_LOOP is deliberately NOT in that set. It is a line strip plus one
 * closing edge, which is a smaller fix than a full expansion, and both
 * backends currently approximate it as a plain strip -- so saying "yes" here
 * would claim an expansion that neither performs. Whoever closes the loop
 * should revisit this, and the test says so. */
static inline int rsx_primitive_needs_expansion(u32 rsx_prim)
{
    return rsx_prim == RSX_PRIMITIVE_QUADS      ||
           rsx_prim == RSX_PRIMITIVE_QUAD_STRIP ||
           rsx_prim == RSX_PRIMITIVE_TRIANGLE_FAN ||
           rsx_prim == RSX_PRIMITIVE_POLYGON;
}

/* The topology this primitive is DRAWN as, after any expansion above.
 *
 * So the expandable types report TRIANGLES: that is what comes out of the
 * expansion, not a claim the hardware understands them. LINE_LOOP reports
 * LINE_STRIP, matching the approximation described above. */
static inline rsx_topology rsx_primitive_topology(u32 rsx_prim)
{
    switch (rsx_prim) {
    case RSX_PRIMITIVE_POINTS:         return RSX_TOPOLOGY_POINTS;
    case RSX_PRIMITIVE_LINES:          return RSX_TOPOLOGY_LINES;
    case RSX_PRIMITIVE_LINE_LOOP:      /* approximated: no closing edge yet */
    case RSX_PRIMITIVE_LINE_STRIP:     return RSX_TOPOLOGY_LINE_STRIP;
    case RSX_PRIMITIVE_TRIANGLE_STRIP: return RSX_TOPOLOGY_TRIANGLE_STRIP;
    case RSX_PRIMITIVE_TRIANGLES:
    case RSX_PRIMITIVE_TRIANGLE_FAN:   /* expanded to a list */
    case RSX_PRIMITIVE_QUADS:
    case RSX_PRIMITIVE_QUAD_STRIP:
    case RSX_PRIMITIVE_POLYGON:        return RSX_TOPOLOGY_TRIANGLES;
    default:                           return RSX_TOPOLOGY_UNSUPPORTED;
    }
}

#ifdef __cplusplus
}
#endif
#endif

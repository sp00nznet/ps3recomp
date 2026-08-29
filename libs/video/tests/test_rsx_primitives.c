/*
 * ps3recomp - self-contained tests for RSX primitive classification
 *
 *   cc -I include -I libs/video -o /tmp/t libs/video/tests/test_rsx_primitives.c && /tmp/t
 *
 * rsx_primitives.h is header-only, so there is nothing to link.
 *
 * This pins a truth table that three places in the tree previously answered
 * differently: the D3D12 backend (bare numbers), the Metal backend (its own
 * list), and a set of helpers in this header that nothing called. The two that
 * were live disagreed about LINE_LOOP and POLYGON.
 */
#include "rsx_primitives.h"

#include <stdio.h>

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
                        g_fail++; } } while (0)

#define CHECK_EQ(got, want)                                                    \
    do { long _g = (long)(got), _w = (long)(want);                             \
         if (_g != _w) { printf("FAIL %s:%d  %s = %ld, want %ld\n",            \
                                __FILE__, __LINE__, #got, _g, _w);             \
                         g_fail++; } } while (0)

/* Every primitive the RSX defines, and what each must come out as. Written as
 * a table so a disagreement is visible rather than buried in control flow. */
static const struct {
    u32          prim;
    const char*  name;
    int          expand;
    rsx_topology topo;
} k_expected[] = {
    { RSX_PRIMITIVE_POINTS,         "POINTS",         0, RSX_TOPOLOGY_POINTS         },
    { RSX_PRIMITIVE_LINES,          "LINES",          0, RSX_TOPOLOGY_LINES          },
    /* LINE_LOOP is a strip plus a closing edge. Neither backend draws that edge
     * yet, so it is NOT reported as needing expansion -- claiming otherwise
     * would promise work nobody does. */
    { RSX_PRIMITIVE_LINE_LOOP,      "LINE_LOOP",      0, RSX_TOPOLOGY_LINE_STRIP     },
    { RSX_PRIMITIVE_LINE_STRIP,     "LINE_STRIP",     0, RSX_TOPOLOGY_LINE_STRIP     },
    { RSX_PRIMITIVE_TRIANGLES,      "TRIANGLES",      0, RSX_TOPOLOGY_TRIANGLES      },
    { RSX_PRIMITIVE_TRIANGLE_STRIP, "TRIANGLE_STRIP", 0, RSX_TOPOLOGY_TRIANGLE_STRIP },
    { RSX_PRIMITIVE_TRIANGLE_FAN,   "TRIANGLE_FAN",   1, RSX_TOPOLOGY_TRIANGLES      },
    { RSX_PRIMITIVE_QUADS,          "QUADS",          1, RSX_TOPOLOGY_TRIANGLES      },
    { RSX_PRIMITIVE_QUAD_STRIP,     "QUAD_STRIP",     1, RSX_TOPOLOGY_TRIANGLES      },
    { RSX_PRIMITIVE_POLYGON,        "POLYGON",        1, RSX_TOPOLOGY_TRIANGLES      },
};

static void test_truth_table(void)
{
    for (unsigned i = 0; i < sizeof k_expected / sizeof k_expected[0]; i++) {
        int  e = rsx_primitive_needs_expansion(k_expected[i].prim);
        rsx_topology t = rsx_primitive_topology(k_expected[i].prim);
        if (e != k_expected[i].expand)
            { printf("FAIL %s: needs_expansion = %d, want %d\n",
                     k_expected[i].name, e, k_expected[i].expand); g_fail++; }
        if (t != k_expected[i].topo)
            { printf("FAIL %s: topology = %d, want %d\n",
                     k_expected[i].name, (int)t, (int)k_expected[i].topo); g_fail++; }
    }
}

/* The contract between the two functions: if a primitive needs expanding, the
 * expansion produces a triangle list, so its topology MUST be TRIANGLES.
 * Anything else would have a backend expand into one topology and then draw
 * with another. */
static void test_expansion_implies_triangles(void)
{
    for (u32 p = 0; p < 16; p++)
        if (rsx_primitive_needs_expansion(p))
            CHECK_EQ(rsx_primitive_topology(p), RSX_TOPOLOGY_TRIANGLES);
}

/* Nothing outside the defined range may claim to be drawable: an unknown
 * primitive that reported TRIANGLES would be silently drawn as garbage rather
 * than skipped. */
static void test_unknown_is_unsupported(void)
{
    CHECK_EQ(rsx_primitive_topology(0),   RSX_TOPOLOGY_UNSUPPORTED);
    CHECK_EQ(rsx_primitive_topology(11),  RSX_TOPOLOGY_UNSUPPORTED);
    CHECK_EQ(rsx_primitive_topology(255), RSX_TOPOLOGY_UNSUPPORTED);
    CHECK(!rsx_primitive_needs_expansion(0));
    CHECK(!rsx_primitive_needs_expansion(11));
    CHECK(!rsx_primitive_needs_expansion(255));
}

/* UNSUPPORTED must stay 0: the D3D12 backend's legacy path tests the returned
 * topology for falsity to decide whether it can draw at all. */
static void test_unsupported_is_zero(void)
{
    CHECK_EQ(RSX_TOPOLOGY_UNSUPPORTED, 0);
}

int main(void)
{
    test_truth_table();
    test_expansion_implies_triangles();
    test_unknown_is_unsupported();
    test_unsupported_is_zero();

    if (g_fail) { printf("\nRSX primitives: %d FAILED\n", g_fail); return 1; }
    printf("RSX primitive tests: all passed\n");
    return 0;
}

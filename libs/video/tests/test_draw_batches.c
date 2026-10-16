#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static long g_live;
static unsigned long g_calls;
static int g_fail_next;

static void* test_realloc(void* ptr, size_t size)
{
    void* q;
    g_calls++;
    if (g_fail_next) {
        g_fail_next = 0;
        return NULL;
    }
    q = realloc(ptr, size);
    if (q != NULL && ptr == NULL)
        g_live++;
    return q;
}

static void test_free(void* ptr)
{
    if (ptr != NULL)
        g_live--;
    free(ptr);
}

#define RSX_DRAW_BATCH_REALLOC(ptr, size) test_realloc((ptr), (size))
#define RSX_DRAW_BATCH_FREE(ptr) test_free(ptr)
#include "../rsx_draw_batches.h"

static unsigned g_checks, g_failures;

#define CHECK(c)                                                              \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(c)) {                                                           \
            g_failures++;                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
        }                                                                     \
    } while (0)

static int is_zeroed(const rsx_draw_batch_list* l)
{
    return l->data == NULL && l->count == 0u && l->capacity == 0u;
}

static void test_null_and_zero(void)
{
    rsx_draw_batch_list l = {0};
    unsigned long calls = g_calls;

    CHECK(rsx_draw_batch_push(NULL, 0u, 1u) == 0);
    rsx_draw_batch_clear(NULL);
    rsx_draw_batch_destroy(NULL);

    CHECK(rsx_draw_batch_push(&l, 123u, 0u) == 1);
    CHECK(is_zeroed(&l));
    CHECK(g_calls == calls);
    CHECK(g_live == 0);

    CHECK(rsx_draw_batch_push(&l, 5u, 2u) == 1);
    CHECK(rsx_draw_batch_push(&l, 9u, 0u) == 1);
    CHECK(l.count == 1u);

    rsx_draw_batch_destroy(&l);
    CHECK(is_zeroed(&l));
    rsx_draw_batch_destroy(&l);
    CHECK(is_zeroed(&l));
    CHECK(g_live == 0);
}

static void test_large_terrain(void)
{
    const uint32_t n = 1025u;
    rsx_draw_batch_list l = {0};
    uint64_t total = 0;
    int pushes_ok = 1, entries_ok = 1;
    uint32_t i;

    for (i = 0; i < n; i++)
        if (!rsx_draw_batch_push(&l, i * 512u + 7u, 256u))
            pushes_ok = 0;
    CHECK(pushes_ok);
    CHECK(l.count == n);
    CHECK(l.count > 256u);
    CHECK(l.capacity >= l.count);
    for (i = 0; i < l.count && i < n; i++) {
        if (l.data[i].first != i * 512u + 7u || l.data[i].count != 256u)
            entries_ok = 0;
        total += l.data[i].count;
    }
    CHECK(entries_ok);
    CHECK(total == 262400u);

    rsx_draw_batch_destroy(&l);
    CHECK(is_zeroed(&l));
    CHECK(g_live == 0);
}

static void test_ordered_no_coalesce(void)
{
    static const rsx_draw_batch want[] = {
        {100u, 3u}, {5u, 7u}, {100u, 3u}, {0u, 4u}, {4u, 4u}, {1000u, 1u}};
    const uint32_t n = (uint32_t)(sizeof(want) / sizeof(want[0]));
    rsx_draw_batch_list l = {0};
    uint32_t i;

    for (i = 0; i < n; i++)
        CHECK(rsx_draw_batch_push(&l, want[i].first, want[i].count) == 1);
    CHECK(l.count == n);
    for (i = 0; i < n && i < l.count; i++)
        CHECK(l.data[i].first == want[i].first && l.data[i].count == want[i].count);
    rsx_draw_batch_destroy(&l);
    CHECK(g_live == 0);
}

static void test_two_collectors(void)
{
    rsx_draw_batch_list indexed = {0}, arrays = {0};
    int ok = 1;
    uint32_t i;

    for (i = 0; i < 600u; i++) {
        if (!rsx_draw_batch_push(&indexed, i * 1000u, 256u))
            ok = 0;
        if (i % 2u == 0u && !rsx_draw_batch_push(&arrays, i, 3u))
            ok = 0;
    }
    CHECK(ok);
    CHECK(indexed.count == 600u);
    CHECK(arrays.count == 300u);
    CHECK(indexed.data != arrays.data);
    for (i = 0; i < indexed.count; i++)
        if (indexed.data[i].first != i * 1000u || indexed.data[i].count != 256u)
            ok = 0;
    for (i = 0; i < arrays.count; i++)
        if (arrays.data[i].first != i * 2u || arrays.data[i].count != 3u)
            ok = 0;
    CHECK(ok);
    CHECK(g_live == 2);

    rsx_draw_batch_destroy(&indexed);
    CHECK(arrays.count == 300u && arrays.data[299].first == 598u);
    rsx_draw_batch_destroy(&arrays);
    CHECK(g_live == 0);
}

static void test_clear_reuse(void)
{
    rsx_draw_batch_list l = {0};
    rsx_draw_batch* data;
    uint32_t cap, i;
    unsigned long calls;
    int ok = 1;

    for (i = 0; i < 700u; i++)
        if (!rsx_draw_batch_push(&l, i, 1u))
            ok = 0;
    CHECK(ok);
    data = l.data;
    cap = l.capacity;
    calls = g_calls;

    rsx_draw_batch_clear(&l);
    CHECK(l.count == 0u);
    CHECK(l.data == data && l.capacity == cap);

    for (i = 0; i < 700u; i++)
        if (!rsx_draw_batch_push(&l, 50000u + i, 2u))
            ok = 0;
    CHECK(ok);
    CHECK(g_calls == calls);
    CHECK(l.data == data && l.capacity == cap && l.count == 700u);
    for (i = 0; i < l.count; i++)
        if (l.data[i].first != 50000u + i || l.data[i].count != 2u)
            ok = 0;
    CHECK(ok);

    rsx_draw_batch_destroy(&l);
    CHECK(g_live == 0);
}

static void test_initial_alloc_failure(void)
{
    rsx_draw_batch_list l = {0};

    g_fail_next = 1;
    CHECK(rsx_draw_batch_push(&l, 1u, 1u) == 0);
    CHECK(is_zeroed(&l));
    CHECK(g_live == 0);

    CHECK(rsx_draw_batch_push(&l, 1u, 1u) == 1);
    CHECK(l.count == 1u && l.data != NULL && l.data[0].first == 1u);
    rsx_draw_batch_destroy(&l);
    CHECK(g_live == 0);
}

static void test_growth_failure(void)
{
    rsx_draw_batch_list l = {0};
    rsx_draw_batch* data;
    uint32_t cap, count, i;
    int ok = 1;

    /* Fill exactly to the current capacity, whatever it is. */
    while (ok && (l.capacity == 0u || l.count < l.capacity))
        ok = rsx_draw_batch_push(&l, l.count * 3u, l.count + 1u);
    CHECK(ok);
    CHECK(l.count == l.capacity && l.count > 0u);
    data = l.data;
    cap = l.capacity;
    count = l.count;

    g_fail_next = 1;
    CHECK(rsx_draw_batch_push(&l, 77u, 1u) == 0);
    CHECK(l.data == data && l.capacity == cap && l.count == count);
    for (i = 0; i < l.count; i++)
        if (l.data[i].first != i * 3u || l.data[i].count != i + 1u)
            ok = 0;
    CHECK(ok);
    CHECK(g_live == 1);

    CHECK(rsx_draw_batch_push(&l, 77u, 1u) == 1);
    CHECK(l.count == count + 1u && l.capacity > cap);
    CHECK(l.data[count].first == 77u && l.data[count].count == 1u);
    for (i = 0; i < count; i++)
        if (l.data[i].first != i * 3u || l.data[i].count != i + 1u)
            ok = 0;
    CHECK(ok);

    rsx_draw_batch_destroy(&l);
    CHECK(g_live == 0);
}

static void test_uint32_bounds(void)
{
    rsx_draw_batch_list l = {0};

    CHECK(rsx_draw_batch_push(&l, UINT32_MAX, 1u) == 1);
    CHECK(rsx_draw_batch_push(&l, 0u, UINT32_MAX) == 1);
    CHECK(rsx_draw_batch_push(&l, 1u, UINT32_MAX) == 1);
    CHECK(l.count == 3u);
    CHECK(rsx_draw_batch_push(&l, UINT32_MAX, 2u) == 0);
    CHECK(rsx_draw_batch_push(&l, 2u, UINT32_MAX) == 0);
    CHECK(rsx_draw_batch_push(&l, UINT32_MAX, UINT32_MAX) == 0);
    CHECK(rsx_draw_batch_push(&l, UINT32_MAX - 9u, 11u) == 0);
    CHECK(l.count == 3u);
    CHECK(l.data[0].first == UINT32_MAX && l.data[0].count == 1u);
    CHECK(l.data[1].first == 0u && l.data[1].count == UINT32_MAX);
    CHECK(l.data[2].first == 1u && l.data[2].count == UINT32_MAX);
    rsx_draw_batch_destroy(&l);
    CHECK(g_live == 0);
}

static void test_invalid_metadata(void)
{
    rsx_draw_batch_list l = {0};
    rsx_draw_batch_list saved;
    unsigned long calls;

    CHECK(rsx_draw_batch_push(&l, 10u, 1u) == 1);
    saved = l;
    calls = g_calls;

    l.count = l.capacity + 1u;
    CHECK(rsx_draw_batch_push(&l, 1u, 1u) == 0);
    CHECK(l.count == saved.capacity + 1u && l.data == saved.data);
    l = saved;

    l.capacity = 0u;
    l.count = 0u;
    CHECK(rsx_draw_batch_push(&l, 1u, 1u) == 0);
    CHECK(l.data == saved.data && l.capacity == 0u);
    l = saved;

    l.data = NULL;
    CHECK(rsx_draw_batch_push(&l, 1u, 1u) == 0);
    CHECK(l.data == NULL && l.count == saved.count);
    l = saved;

    /* Full uint32 list: must reject without allocating or writing. */
    l.count = UINT32_MAX;
    l.capacity = UINT32_MAX;
    CHECK(rsx_draw_batch_push(&l, 1u, 1u) == 0);
    CHECK(l.count == UINT32_MAX && l.capacity == UINT32_MAX && l.data == saved.data);
    l = saved;

    CHECK(g_calls == calls);
    CHECK(l.count == 1u && l.data[0].first == 10u && l.data[0].count == 1u);
    CHECK(rsx_draw_batch_push(&l, 11u, 1u) == 1);
    CHECK(l.count == 2u && l.data[1].first == 11u);

    rsx_draw_batch_destroy(&l);
    rsx_draw_batch_destroy(&l);
    CHECK(is_zeroed(&l));
    CHECK(g_live == 0);
}

int main(void)
{
    test_null_and_zero();
    test_large_terrain();
    test_ordered_no_coalesce();
    test_two_collectors();
    test_clear_reuse();
    test_initial_alloc_failure();
    test_growth_failure();
    test_uint32_bounds();
    test_invalid_metadata();
    CHECK(g_live == 0);

    printf("test_draw_batches: %u checks, %u failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

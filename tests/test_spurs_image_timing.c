#include "../libs/spurs/spurs_image_timing.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                           \
        }                                                                         \
    } while (0)

#define TEST_LINE_CAP 512
#define TEST_MAX_LINES 40

static char lines[TEST_MAX_LINES][TEST_LINE_CAP];

static size_t read_lines(FILE *f)
{
    char buf[TEST_LINE_CAP];
    size_t n = 0;
    rewind(f);
    while (fgets(buf, sizeof buf, f)) {
        if (n < TEST_MAX_LINES)
            memcpy(lines[n], buf, sizeof buf);
        n++;
    }
    return n;
}

static const struct spurs_image_timing_entry *find(const struct spurs_image_timing *t,
                                                   uint64_t fp, uint32_t size)
{
    uint32_t i;
    for (i = 0; i < t->used; ++i)
        if (t->entries[i].fingerprint == fp && t->entries[i].image_size == size)
            return &t->entries[i];
    return NULL;
}

static void fill_capacity(struct spurs_image_timing *t)
{
    uint32_t i;
    for (i = 0; i < SPURS_IMAGE_TIMING_CAPACITY; ++i)
        spurs_image_timing_add(t, (uint64_t)i + 1u, 256u, 1u);
}

static void test_reset_and_empty_emit(void)
{
    struct spurs_image_timing t;
    FILE *f;
    memset(&t, 0xA5, sizeof t);
    spurs_image_timing_reset(&t);
    CHECK(t.used == 0);
    CHECK(t.saturated == 0);
    CHECK(t.overflow.calls == 0 && t.unknown.calls == 0);
    CHECK(t.entries[0].calls == 0 && t.entries[31].dispatch_us == 0);

    f = tmpfile();
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(spurs_image_timing_emit(&t, f, 1u, 0u, 5000000u) == 0);
    CHECK(read_lines(f) == 0);

    spurs_image_timing_add(&t, 9u, 9u, 9u);
    spurs_image_timing_reset(&t);
    CHECK(spurs_image_timing_emit(&t, f, 1u, 0u, 5000000u) == 0);
    CHECK(read_lines(f) == 0);
    fclose(f);

    spurs_image_timing_reset(NULL);
    spurs_image_timing_add(NULL, 1u, 1u, 1u);
}

static void test_stable_keying_and_totals(void)
{
    struct spurs_image_timing t;
    const struct spurs_image_timing_entry *e;
    spurs_image_timing_reset(&t);
    spurs_image_timing_add(&t, 0xAAu, 100u, 10u);
    spurs_image_timing_add(&t, 0xAAu, 100u, 30u);
    spurs_image_timing_add(&t, 0xAAu, 100u, 10u);
    CHECK(t.used == 1);
    e = find(&t, 0xAAu, 100u);
    CHECK(e != NULL);
    if (e) {
        CHECK(e->calls == 3);
        CHECK(e->dispatch_us == 50);
        CHECK(e->max_us == 30);
    }
    CHECK(t.saturated == 0);
}

static void test_distinct_keys(void)
{
    struct spurs_image_timing t;
    const struct spurs_image_timing_entry *a;
    const struct spurs_image_timing_entry *b;
    const struct spurs_image_timing_entry *c;
    spurs_image_timing_reset(&t);
    /* A reused descriptor carrying different admitted images: only the
     * fingerprint differs, so the keys must stay separate. */
    spurs_image_timing_add(&t, 0x1111u, 4096u, 5u);
    spurs_image_timing_add(&t, 0x2222u, 4096u, 7u);
    /* Same fingerprint, different size: separate keys. */
    spurs_image_timing_add(&t, 0x1111u, 8192u, 11u);
    CHECK(t.used == 3);
    a = find(&t, 0x1111u, 4096u);
    b = find(&t, 0x2222u, 4096u);
    c = find(&t, 0x1111u, 8192u);
    CHECK(a && b && c);
    if (a && b && c) {
        CHECK(a->calls == 1 && a->dispatch_us == 5);
        CHECK(b->calls == 1 && b->dispatch_us == 7);
        CHECK(c->calls == 1 && c->dispatch_us == 11);
    }
    CHECK(t.entries[0].fingerprint == 0x1111u && t.entries[0].image_size == 4096u);
    CHECK(t.entries[1].fingerprint == 0x2222u);
    CHECK(t.entries[2].image_size == 8192u);
}

static void test_zero_fingerprint_and_unknown(void)
{
    struct spurs_image_timing t;
    const struct spurs_image_timing_entry *z;
    spurs_image_timing_reset(&t);
    spurs_image_timing_add(&t, 0u, 64u, 3u);
    CHECK(t.used == 1);
    z = find(&t, 0u, 64u);
    CHECK(z != NULL && z->calls == 1 && z->dispatch_us == 3);
    CHECK(t.unknown.calls == 0);

    spurs_image_timing_add(&t, 0u, 0u, 4u);
    spurs_image_timing_add(&t, 0x55u, 0u, 9u);
    CHECK(t.used == 1);
    CHECK(t.unknown.calls == 2);
    CHECK(t.unknown.dispatch_us == 13);
    CHECK(t.unknown.max_us == 9);
    CHECK(t.unknown.fingerprint == 0 && t.unknown.image_size == 0);
    CHECK(z != NULL && z->calls == 1 && z->dispatch_us == 3);
    CHECK(t.overflow.calls == 0);
}

static void test_capacity_and_overflow(void)
{
    struct spurs_image_timing t;
    uint32_t i;
    spurs_image_timing_reset(&t);
    fill_capacity(&t);
    CHECK(t.used == SPURS_IMAGE_TIMING_CAPACITY);
    for (i = 0; i < SPURS_IMAGE_TIMING_CAPACITY; ++i)
        CHECK(t.entries[i].fingerprint == (uint64_t)i + 1u);
    CHECK(t.overflow.calls == 0);

    spurs_image_timing_add(&t, 1000u, 256u, 40u);
    spurs_image_timing_add(&t, 1001u, 256u, 60u);
    CHECK(t.used == SPURS_IMAGE_TIMING_CAPACITY);
    CHECK(t.overflow.calls == 2);
    CHECK(t.overflow.dispatch_us == 100);
    CHECK(t.overflow.max_us == 60);
    CHECK(find(&t, 1000u, 256u) == NULL);

    /* Existing keys still update after capacity is reached. */
    spurs_image_timing_add(&t, 1u, 256u, 99u);
    CHECK(t.entries[0].calls == 2);
    CHECK(t.entries[0].dispatch_us == 100);
    CHECK(t.entries[0].max_us == 99);
    CHECK(t.overflow.calls == 2);

    /* Same fingerprint as a stored key but new size overflows, no
     * replacement. */
    spurs_image_timing_add(&t, 1u, 512u, 1u);
    CHECK(t.overflow.calls == 3);
    CHECK(t.entries[0].image_size == 256u);

    /* Unknown still separate when full. */
    spurs_image_timing_add(&t, 1u, 0u, 2u);
    CHECK(t.unknown.calls == 1);
    CHECK(t.overflow.calls == 3);
    for (i = 0; i < SPURS_IMAGE_TIMING_CAPACITY; ++i)
        CHECK(t.entries[i].fingerprint == (uint64_t)i + 1u);
    CHECK(t.saturated == 0);
}

static void test_saturation(void)
{
    struct spurs_image_timing t;
    spurs_image_timing_reset(&t);

    spurs_image_timing_add(&t, 7u, 64u, UINT64_MAX - 5u);
    spurs_image_timing_add(&t, 7u, 64u, 5u);
    CHECK(t.entries[0].dispatch_us == UINT64_MAX);
    CHECK(t.saturated == 0);
    spurs_image_timing_add(&t, 7u, 64u, 0u);
    CHECK(t.saturated == 0);
    spurs_image_timing_add(&t, 7u, 64u, 1u);
    CHECK(t.entries[0].dispatch_us == UINT64_MAX);
    CHECK(t.entries[0].calls == 4);
    CHECK(t.entries[0].max_us == UINT64_MAX - 5u);
    CHECK(t.saturated != 0);
    spurs_image_timing_add(&t, 8u, 64u, 1u);
    CHECK(t.saturated != 0);

    spurs_image_timing_reset(&t);
    CHECK(t.saturated == 0);
    spurs_image_timing_add(&t, 7u, 64u, 1u);
    t.entries[0].calls = UINT64_MAX - 1u;
    spurs_image_timing_add(&t, 7u, 64u, 1u);
    CHECK(t.entries[0].calls == UINT64_MAX);
    CHECK(t.saturated == 0);
    spurs_image_timing_add(&t, 7u, 64u, 1u);
    CHECK(t.entries[0].calls == UINT64_MAX);
    CHECK(t.entries[0].dispatch_us == 3);
    CHECK(t.saturated != 0);

    spurs_image_timing_reset(&t);
    fill_capacity(&t);
    spurs_image_timing_add(&t, 500u, 1u, UINT64_MAX - 1u);
    CHECK(t.saturated == 0);
    spurs_image_timing_add(&t, 501u, 1u, 2u);
    CHECK(t.overflow.dispatch_us == UINT64_MAX);
    CHECK(t.overflow.calls == 2);
    CHECK(t.saturated != 0);

    spurs_image_timing_reset(&t);
    spurs_image_timing_add(&t, 0u, 0u, UINT64_MAX);
    CHECK(t.saturated == 0);
    spurs_image_timing_add(&t, 0u, 0u, 1u);
    CHECK(t.unknown.dispatch_us == UINT64_MAX);
    CHECK(t.unknown.max_us == UINT64_MAX);
    CHECK(t.saturated != 0);
    spurs_image_timing_reset(&t);
    CHECK(t.saturated == 0 && t.unknown.calls == 0);
}

static void test_emit_fields(void)
{
    struct spurs_image_timing t;
    FILE *f;
    size_t n;
    spurs_image_timing_reset(&t);
    spurs_image_timing_add(&t, 0x0123456789ABCDEFull, 4096u, 10u);
    spurs_image_timing_add(&t, 0x0123456789ABCDEFull, 4096u, 20u);
    spurs_image_timing_add(&t, 0u, 128u, 7u);
    spurs_image_timing_add(&t, 0xFFu, 0u, 3u);

    f = tmpfile();
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(spurs_image_timing_emit(&t, f, 0xABCDu, 1000u, 5001000u) == 0);
    n = read_lines(f);
    CHECK(n == 3);
    if (n == 3) {
        CHECK(strcmp(lines[0],
                     "[spurs-image-window] chain=0x0000ABCD start_us=1000 end_us=5001000 "
                     "fp=0x0123456789ABCDEF image_size=4096 calls=2 dispatch_us=30 "
                     "max_us=20 bucket=known saturated=0\n") == 0);
        CHECK(strcmp(lines[1],
                     "[spurs-image-window] chain=0x0000ABCD start_us=1000 end_us=5001000 "
                     "fp=0x0000000000000000 image_size=128 calls=1 dispatch_us=7 "
                     "max_us=7 bucket=known saturated=0\n") == 0);
        CHECK(strcmp(lines[2],
                     "[spurs-image-window] chain=0x0000ABCD start_us=1000 end_us=5001000 "
                     "fp=0x0000000000000000 image_size=0 calls=1 dispatch_us=3 "
                     "max_us=3 bucket=unknown saturated=0\n") == 0);
    }
    fclose(f);

    /* Emission does not consume the aggregate. */
    CHECK(t.used == 2 && t.entries[0].calls == 2);

    /* Saturation flag appears on every line. */
    spurs_image_timing_add(&t, 0u, 128u, UINT64_MAX);
    CHECK(t.saturated != 0);
    f = tmpfile();
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(spurs_image_timing_emit(&t, f, 0xFFFFFFFFu, 0u, 0u) == 0);
    n = read_lines(f);
    CHECK(n == 3);
    if (n == 3) {
        CHECK(strstr(lines[0], "chain=0xFFFFFFFF start_us=0 end_us=0 ") != NULL);
        CHECK(strstr(lines[0], " saturated=1\n") != NULL);
        CHECK(strstr(lines[1], " dispatch_us=18446744073709551615 "
                               "max_us=18446744073709551615 bucket=known saturated=1\n") != NULL);
        CHECK(strstr(lines[2], "bucket=unknown saturated=1\n") != NULL);
    }
    fclose(f);
}

static void test_emit_bounds(void)
{
    struct spurs_image_timing t;
    FILE *f;
    size_t n;
    size_t i;
    char expect_fp[32];
    spurs_image_timing_reset(&t);
    fill_capacity(&t);
    spurs_image_timing_add(&t, 900u, 256u, 4u);
    spurs_image_timing_add(&t, 901u, 256u, 6u);
    spurs_image_timing_add(&t, 902u, 0u, 8u);
    t.entries[5].calls = UINT64_MAX;
    t.entries[5].dispatch_us = UINT64_MAX;
    t.entries[5].max_us = UINT64_MAX;
    t.entries[5].fingerprint = UINT64_MAX;
    t.entries[5].image_size = UINT32_MAX;

    f = tmpfile();
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(spurs_image_timing_emit(&t, f, 0xFFFFFFFFu, UINT64_MAX - 1u, UINT64_MAX) == 0);
    n = read_lines(f);
    CHECK(n == SPURS_IMAGE_TIMING_CAPACITY + 2u);
    if (n == SPURS_IMAGE_TIMING_CAPACITY + 2u) {
        for (i = 0; i < n; ++i) {
            size_t len = strlen(lines[i]);
            CHECK(len > 0 && len < 300);
            CHECK(lines[i][len - 1] == '\n');
            CHECK(strncmp(lines[i], "[spurs-image-window] chain=0xFFFFFFFF ", 38) == 0);
        }
        for (i = 0; i < SPURS_IMAGE_TIMING_CAPACITY; ++i) {
            CHECK(strstr(lines[i], " bucket=known ") != NULL);
            if (i != 5) {
                snprintf(expect_fp, sizeof expect_fp, " fp=0x%016X ", (unsigned)(i + 1u));
                CHECK(strstr(lines[i], expect_fp) != NULL);
            }
        }
        CHECK(strstr(lines[5], " fp=0xFFFFFFFFFFFFFFFF image_size=4294967295 "
                               "calls=18446744073709551615 ") != NULL);
        CHECK(strstr(lines[32], " image_size=0 calls=2 dispatch_us=10 max_us=6 "
                                "bucket=overflow ") != NULL);
        CHECK(strstr(lines[33], " calls=1 dispatch_us=8 max_us=8 bucket=unknown ") != NULL);
    }
    fclose(f);
}

static void test_emit_invalid(void)
{
    struct spurs_image_timing t;
    FILE *f;
    spurs_image_timing_reset(&t);
    spurs_image_timing_add(&t, 1u, 1u, 1u);
    f = tmpfile();
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(spurs_image_timing_emit(NULL, f, 0u, 0u, 1u) == -1);
    CHECK(spurs_image_timing_emit(&t, NULL, 0u, 0u, 1u) == -1);
    CHECK(spurs_image_timing_emit(&t, f, 0u, 2u, 1u) == -1);
    CHECK(spurs_image_timing_emit(&t, f, 0u, UINT64_MAX, 0u) == -1);
    CHECK(read_lines(f) == 0);
    CHECK(spurs_image_timing_emit(&t, f, 0u, 5u, 5u) == 0);
    CHECK(read_lines(f) == 1);
    fclose(f);
}

int main(void)
{
    test_reset_and_empty_emit();
    test_stable_keying_and_totals();
    test_distinct_keys();
    test_zero_fingerprint_and_unknown();
    test_capacity_and_overflow();
    test_saturation();
    test_emit_fields();
    test_emit_bounds();
    test_emit_invalid();
    if (failures) {
        fprintf(stderr, "test_spurs_image_timing: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_spurs_image_timing: ok\n");
    return 0;
}

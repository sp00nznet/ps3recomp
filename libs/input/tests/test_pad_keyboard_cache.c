#include "../pad_keyboard_cache.h"

#include <stdint.h>
#include <stdio.h>

static unsigned g_checks;
static unsigned g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        uint32_t a_ = (uint32_t)(actual);                                    \
        uint32_t e_ = (uint32_t)(expected);                                  \
        g_checks++;                                                          \
        if (a_ != e_) {                                                      \
            g_failures++;                                                    \
            printf("FAIL %s:%d: %s == 0x%08x, expected 0x%08x\n", __FILE__, \
                   __LINE__, #actual, (unsigned)a_, (unsigned)e_);           \
        }                                                                    \
    } while (0)

#define F PAD_KEYBOARD_FOCUSED
#define WIN 7u

static int invariant_ok(uint32_t v)
{
    if (v & ~(uint32_t)PAD_KEYBOARD_VALID_MASK)
        return 0;
    return v == 0 || (v & F) != 0;
}

static void focused_cache(PadKeyboardCache* c, uint32_t id)
{
    pad_keyboard_cache_attach(c, id);
    pad_keyboard_cache_focus(c, id, 1);
}

static void test_initial_and_null(void)
{
    PadKeyboardCache c = {0};
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    CHECK_EQ(pad_keyboard_cache_read(NULL), 0);

    pad_keyboard_cache_attach(NULL, WIN);
    pad_keyboard_cache_detach(NULL);
    pad_keyboard_cache_focus(NULL, WIN, 1);
    pad_keyboard_cache_key(NULL, WIN, 1u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(NULL), 0);

    /* Unattached cache (window 0) ignores everything. */
    pad_keyboard_cache_focus(&c, 0, 1);
    pad_keyboard_cache_key(&c, 0, 1u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
}

static void test_window_ids(void)
{
    PadKeyboardCache c = {0};

    pad_keyboard_cache_attach(&c, 0);
    pad_keyboard_cache_focus(&c, 0, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    pad_keyboard_cache_key(&c, 0, 1u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);

    pad_keyboard_cache_attach(&c, UINT32_MAX);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    pad_keyboard_cache_focus(&c, UINT32_MAX - 1u, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    pad_keyboard_cache_focus(&c, 0, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    pad_keyboard_cache_focus(&c, UINT32_MAX, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c), F);
    pad_keyboard_cache_key(&c, UINT32_MAX, 0x8000u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x8000u);

    /* Different window events are ignored. */
    pad_keyboard_cache_key(&c, 1u, 0x0001u, 1, 0);
    pad_keyboard_cache_key(&c, 0, 0x0001u, 1, 0);
    pad_keyboard_cache_key(&c, UINT32_MAX, 0x8000u, 0, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F);
    pad_keyboard_cache_key(&c, UINT32_MAX, 0x0002u, 1, 0);
    pad_keyboard_cache_focus(&c, 1u, 0);
    pad_keyboard_cache_focus(&c, 0, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x0002u);
}

static void test_all_keys(void)
{
    PadKeyboardCache c = {0};
    uint32_t expected = F;
    unsigned bit;

    focused_cache(&c, WIN);
    for (bit = 0; bit < 16; ++bit) {
        uint16_t b = (uint16_t)(1u << bit);
        PadKeyboardCache single = {0};
        focused_cache(&single, WIN);
        pad_keyboard_cache_key(&single, WIN, b, 1, 0);
        CHECK_EQ(pad_keyboard_cache_read(&single), F | b);
        pad_keyboard_cache_key(&single, WIN, b, 0, 0);
        CHECK_EQ(pad_keyboard_cache_read(&single), F);

        pad_keyboard_cache_key(&c, WIN, b, 1, 0);
        expected |= b;
        CHECK_EQ(pad_keyboard_cache_read(&c), expected);
    }
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0xFFFFu);
    for (bit = 0; bit < 16; ++bit) {
        uint16_t b = (uint16_t)(1u << bit);
        pad_keyboard_cache_key(&c, WIN, b, 0, 0);
        expected &= ~(uint32_t)b;
        CHECK_EQ(pad_keyboard_cache_read(&c), expected);
    }
    CHECK_EQ(pad_keyboard_cache_read(&c), F);
}

static void test_button_rejection(void)
{
    PadKeyboardCache c = {0};
    static const uint16_t bad[] = {0x0000u, 0x0003u, 0x8001u, 0xFFFFu, 0x0300u, 0x5555u};
    size_t i;

    focused_cache(&c, WIN);
    for (i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        pad_keyboard_cache_key(&c, WIN, bad[i], 1, 0);
        CHECK_EQ(pad_keyboard_cache_read(&c), F);
    }

    pad_keyboard_cache_key(&c, WIN, 0x0001u, 1, 0);
    pad_keyboard_cache_key(&c, WIN, 0x0002u, 1, 0);
    for (i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        pad_keyboard_cache_key(&c, WIN, bad[i], 0, 0);
        CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x0003u);
    }
}

static void test_invalid_flags(void)
{
    PadKeyboardCache c = {0};
    static const int bad[] = {-1, 2, 255, -2147483647 - 1, 2147483647};
    size_t i;

    focused_cache(&c, WIN);
    for (i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        pad_keyboard_cache_key(&c, WIN, 0x0010u, bad[i], 0);
        CHECK_EQ(pad_keyboard_cache_read(&c), F);
        pad_keyboard_cache_key(&c, WIN, 0x0010u, 1, bad[i]);
        CHECK_EQ(pad_keyboard_cache_read(&c), F);
    }

    pad_keyboard_cache_key(&c, WIN, 0x0010u, 1, 0);
    for (i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        pad_keyboard_cache_key(&c, WIN, 0x0010u, bad[i], 0);
        pad_keyboard_cache_key(&c, WIN, 0x0010u, 0, bad[i]);
        CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x0010u);
        pad_keyboard_cache_focus(&c, WIN, bad[i]);
        CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x0010u);
    }

    /* Invalid focus values on an unfocused cache also do nothing. */
    pad_keyboard_cache_focus(&c, WIN, 0);
    for (i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        pad_keyboard_cache_focus(&c, WIN, bad[i]);
        CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    }
}

static void test_simultaneous_keys(void)
{
    PadKeyboardCache c = {0};

    focused_cache(&c, WIN);
    pad_keyboard_cache_key(&c, WIN, 0x0001u, 1, 0);
    pad_keyboard_cache_key(&c, WIN, 0x0040u, 1, 0);
    pad_keyboard_cache_key(&c, WIN, 0x4000u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x4041u);

    pad_keyboard_cache_key(&c, WIN, 0x0040u, 0, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x4001u);
    /* Release of a key that is not held changes nothing. */
    pad_keyboard_cache_key(&c, WIN, 0x0040u, 0, 0);
    pad_keyboard_cache_key(&c, WIN, 0x0100u, 0, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x4001u);
    /* Duplicate fresh keydown is idempotent. */
    pad_keyboard_cache_key(&c, WIN, 0x0001u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x4001u);
    pad_keyboard_cache_key(&c, WIN, 0x0001u, 0, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x4000u);
    pad_keyboard_cache_key(&c, WIN, 0x4000u, 0, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F);
}

static void test_repeat_rules(void)
{
    PadKeyboardCache c = {0};

    focused_cache(&c, WIN);
    pad_keyboard_cache_key(&c, WIN, 0x0004u, 1, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c), F);

    pad_keyboard_cache_key(&c, WIN, 0x0004u, 1, 0);
    pad_keyboard_cache_key(&c, WIN, 0x0004u, 1, 1);
    pad_keyboard_cache_key(&c, WIN, 0x0004u, 1, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x0004u);

    /* Keyup with repeat flag still clears. */
    pad_keyboard_cache_key(&c, WIN, 0x0004u, 0, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c), F);
}

static void test_focus_rules(void)
{
    PadKeyboardCache c = {0};

    pad_keyboard_cache_attach(&c, WIN);
    /* Keys while unfocused are ignored. */
    pad_keyboard_cache_key(&c, WIN, 0x0001u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);

    pad_keyboard_cache_focus(&c, WIN, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c), F);
    pad_keyboard_cache_key(&c, WIN, 0x0001u, 1, 0);
    pad_keyboard_cache_key(&c, WIN, 0x0800u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x0801u);

    /* Duplicate focus gain preserves held keys. */
    pad_keyboard_cache_focus(&c, WIN, 1);
    pad_keyboard_cache_focus(&c, WIN, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x0801u);

    /* Focus loss clears everything. */
    pad_keyboard_cache_focus(&c, WIN, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    pad_keyboard_cache_focus(&c, WIN, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    pad_keyboard_cache_key(&c, WIN, 0x0001u, 1, 0);
    pad_keyboard_cache_key(&c, WIN, 0x0001u, 0, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);

    /* Refocus starts with no buttons; repeats of a physically held key cannot reactivate. */
    pad_keyboard_cache_focus(&c, WIN, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c), F);
    pad_keyboard_cache_key(&c, WIN, 0x0001u, 1, 1);
    pad_keyboard_cache_key(&c, WIN, 0x0800u, 1, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c), F);
    pad_keyboard_cache_key(&c, WIN, 0x0001u, 0, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F);
    pad_keyboard_cache_key(&c, WIN, 0x0001u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x0001u);
}

static void test_attach_detach(void)
{
    PadKeyboardCache c = {0};

    focused_cache(&c, 10u);
    pad_keyboard_cache_key(&c, 10u, 0x0020u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x0020u);

    /* New window discards old keys and rejects old window events. */
    pad_keyboard_cache_attach(&c, 11u);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    pad_keyboard_cache_focus(&c, 10u, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    pad_keyboard_cache_focus(&c, 11u, 1);
    pad_keyboard_cache_key(&c, 10u, 0x0020u, 1, 0);
    pad_keyboard_cache_key(&c, 11u, 0x0040u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x0040u);
    pad_keyboard_cache_focus(&c, 10u, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x0040u);

    /* Reattaching the same window still discards state. */
    pad_keyboard_cache_attach(&c, 11u);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    pad_keyboard_cache_key(&c, 11u, 0x0040u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);

    /* Detach publishes 0 and disables everything. */
    pad_keyboard_cache_focus(&c, 11u, 1);
    pad_keyboard_cache_key(&c, 11u, 0x0040u, 1, 0);
    pad_keyboard_cache_detach(&c);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    CHECK_EQ(c.window_id, 0);
    pad_keyboard_cache_focus(&c, 11u, 1);
    pad_keyboard_cache_key(&c, 11u, 0x0040u, 1, 0);
    pad_keyboard_cache_focus(&c, 0, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    pad_keyboard_cache_detach(&c);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);

    /* Reattach works after detach. */
    pad_keyboard_cache_attach(&c, 12u);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    pad_keyboard_cache_focus(&c, 12u, 1);
    pad_keyboard_cache_key(&c, 12u, 0x0080u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), F | 0x0080u);

    /* Attach to ID 0 disables input. */
    pad_keyboard_cache_attach(&c, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
    pad_keyboard_cache_focus(&c, 0, 1);
    pad_keyboard_cache_key(&c, 0, 0x0080u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&c), 0);
}

static void test_independent_instances(void)
{
    PadKeyboardCache a = {0};
    PadKeyboardCache b = {0};

    focused_cache(&a, WIN);
    focused_cache(&b, WIN);
    pad_keyboard_cache_key(&a, WIN, 0x0001u, 1, 0);
    pad_keyboard_cache_key(&b, WIN, 0x0002u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&a), F | 0x0001u);
    CHECK_EQ(pad_keyboard_cache_read(&b), F | 0x0002u);

    pad_keyboard_cache_focus(&a, WIN, 0);
    CHECK_EQ(pad_keyboard_cache_read(&a), 0);
    CHECK_EQ(pad_keyboard_cache_read(&b), F | 0x0002u);

    pad_keyboard_cache_detach(&b);
    pad_keyboard_cache_focus(&a, WIN, 1);
    pad_keyboard_cache_key(&a, WIN, 0x0004u, 1, 0);
    CHECK_EQ(pad_keyboard_cache_read(&a), F | 0x0004u);
    CHECK_EQ(pad_keyboard_cache_read(&b), 0);
}

/* Simple reference model, written independently of the header. */
typedef struct Model {
    uint32_t window;
    int focused;
    uint32_t buttons;
} Model;

static uint32_t model_value(const Model* m)
{
    return m->focused ? (F | m->buttons) : 0u;
}

static uint32_t rng_state = 0x12345678u;

static uint32_t rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void test_mixed_sequence(void)
{
    static const uint32_t ids[] = {0u, 1u, 2u, UINT32_MAX};
    static const int flags[] = {0, 1, 0, 1, 0, 1, 2, -1};
    PadKeyboardCache c = {0};
    Model m = {0, 0, 0};
    unsigned mismatches = 0;
    unsigned bad_invariant = 0;
    unsigned i;

    for (i = 0; i < 20000u; ++i) {
        uint32_t op = rng() % 100u;
        uint32_t id = ids[rng() % 4u];
        uint32_t v;

        if (op < 2u) {
            pad_keyboard_cache_attach(&c, id);
            m.window = id;
            m.focused = 0;
            m.buttons = 0;
        } else if (op < 3u) {
            pad_keyboard_cache_detach(&c);
            m.window = 0;
            m.focused = 0;
            m.buttons = 0;
        } else if (op < 15u) {
            int f = flags[rng() % 8u];
            pad_keyboard_cache_focus(&c, id, f);
            if (id != 0 && id == m.window && (f == 0 || f == 1)) {
                if (!f) {
                    m.focused = 0;
                    m.buttons = 0;
                } else if (!m.focused) {
                    m.focused = 1;
                    m.buttons = 0;
                }
            }
        } else {
            uint16_t button;
            int pressed = flags[rng() % 8u];
            int repeat = flags[rng() % 8u];
            uint32_t r = rng() % 20u;
            if (r < 16u)
                button = (uint16_t)(1u << r);
            else if (r == 16u)
                button = 0;
            else
                button = (uint16_t)(rng() | 0x0101u);
            /* Bias toward the attached window so key paths get exercised. */
            if (rng() % 4u != 0u)
                id = m.window;
            pad_keyboard_cache_key(&c, id, button, pressed, repeat);
            if (id != 0 && id == m.window && m.focused &&
                (pressed == 0 || pressed == 1) && (repeat == 0 || repeat == 1) &&
                button != 0 && (button & (button - 1u)) == 0) {
                if (pressed && !repeat)
                    m.buttons |= button;
                else if (!pressed)
                    m.buttons &= ~(uint32_t)button;
            }
        }

        v = pad_keyboard_cache_read(&c);
        if (!invariant_ok(v))
            bad_invariant++;
        if (v != model_value(&m))
            mismatches++;
        /* Keep the model attached to something focusable most of the time. */
        if (m.window == 0 && rng() % 8u == 0u) {
            uint32_t nid = ids[1u + rng() % 3u];
            pad_keyboard_cache_attach(&c, nid);
            pad_keyboard_cache_focus(&c, nid, 1);
            m.window = nid;
            m.focused = 1;
            m.buttons = 0;
        }
    }
    CHECK_EQ(bad_invariant, 0);
    CHECK_EQ(mismatches, 0);
}

static void test_no_high_bits(void)
{
    PadKeyboardCache c = {0};
    unsigned bit;

    focused_cache(&c, WIN);
    for (bit = 0; bit < 16; ++bit)
        pad_keyboard_cache_key(&c, WIN, (uint16_t)(1u << bit), 1, 0);
    pad_keyboard_cache_focus(&c, WIN, 1);
    CHECK_EQ(pad_keyboard_cache_read(&c) & ~(uint32_t)PAD_KEYBOARD_VALID_MASK, 0);
    CHECK(invariant_ok(pad_keyboard_cache_read(&c)));
    CHECK_EQ(PAD_KEYBOARD_FOCUSED, 0x10000u);
}

int main(void)
{
    test_initial_and_null();
    test_window_ids();
    test_all_keys();
    test_button_rejection();
    test_invalid_flags();
    test_simultaneous_keys();
    test_repeat_rules();
    test_focus_rules();
    test_attach_detach();
    test_independent_instances();
    test_no_high_bits();
    test_mixed_sequence();

    printf("pad_keyboard_cache: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

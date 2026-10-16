#ifndef FOLKLORE_PAD_KEYBOARD_CACHE_H
#define FOLKLORE_PAD_KEYBOARD_CACHE_H

/*
 * Keyboard button cache carried from the native window (UI) thread to the
 * guest input polling threads.
 *
 * Threading contract:
 *  - State is zero-initialized ({0}) before any thread starts.
 *  - Exactly one UI thread calls attach/detach/focus/key. window_id and the
 *    writer's view of the snapshot are only touched by that thread.
 *  - Any number of guest threads may call pad_keyboard_cache_read(), which
 *    only acquire-loads the snapshot. Each published snapshot is an
 *    independent, self-consistent value: low 16 bits are pressed buttons,
 *    bit 16 is focus. Buttons are never published without focus.
 *
 * The cache never synthesizes events; the coordinator maps SDL keys to
 * button bits and forwards real events here.
 */

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define PAD_KEYBOARD_FOCUSED (1u << 16)
#define PAD_KEYBOARD_BUTTON_MASK 0xFFFFu
#define PAD_KEYBOARD_VALID_MASK (PAD_KEYBOARD_FOCUSED | PAD_KEYBOARD_BUTTON_MASK)

_Static_assert(sizeof(unsigned int) >= 4, "atomic_uint must hold 17 bits");

typedef struct PadKeyboardCache {
    atomic_uint snapshot;
    uint32_t window_id;
} PadKeyboardCache;

/* Writer-only: the single writer may read its own last publication relaxed. */
static inline uint32_t pad_keyboard_cache_current_(const PadKeyboardCache* state)
{
    return (uint32_t)atomic_load_explicit(&((PadKeyboardCache*)state)->snapshot,
                                          memory_order_relaxed);
}

static inline void pad_keyboard_cache_publish_(PadKeyboardCache* state, uint32_t value)
{
    value &= PAD_KEYBOARD_VALID_MASK;
    if (!(value & PAD_KEYBOARD_FOCUSED))
        value = 0;
    atomic_store_explicit(&state->snapshot, (unsigned int)value, memory_order_release);
}

/* Binds the cache to a window, discarding all published state. ID 0 disables input. */
static inline void pad_keyboard_cache_attach(PadKeyboardCache* state, uint32_t window_id)
{
    if (state == NULL)
        return;
    state->window_id = window_id;
    atomic_store_explicit(&state->snapshot, 0u, memory_order_release);
}

static inline void pad_keyboard_cache_detach(PadKeyboardCache* state)
{
    if (state == NULL)
        return;
    state->window_id = 0;
    atomic_store_explicit(&state->snapshot, 0u, memory_order_release);
}

static inline void pad_keyboard_cache_focus(PadKeyboardCache* state, uint32_t window_id, int focused)
{
    if (state == NULL || window_id == 0 || window_id != state->window_id)
        return;
    if (focused != 0 && focused != 1)
        return;

    if (!focused) {
        /* Focus loss releases everything so keys cannot stick. */
        pad_keyboard_cache_publish_(state, 0);
        return;
    }

    /* Duplicate gain keeps held keys; a fresh gain starts with no buttons. */
    if (pad_keyboard_cache_current_(state) & PAD_KEYBOARD_FOCUSED)
        return;
    pad_keyboard_cache_publish_(state, PAD_KEYBOARD_FOCUSED);
}

static inline void pad_keyboard_cache_key(PadKeyboardCache* state, uint32_t window_id,
                                          uint16_t button, int pressed, int repeat)
{
    uint32_t current;
    uint32_t next;

    if (state == NULL || window_id == 0 || window_id != state->window_id)
        return;
    if ((pressed != 0 && pressed != 1) || (repeat != 0 && repeat != 1))
        return;
    if (button == 0 || (button & (uint16_t)(button - 1u)) != 0)
        return;

    current = pad_keyboard_cache_current_(state);
    if (!(current & PAD_KEYBOARD_FOCUSED))
        return;

    if (pressed) {
        /* Only a fresh keydown counts; repeats (e.g. a key held across refocus)
         * must not activate a button. */
        if (repeat)
            return;
        next = current | (uint32_t)button;
    } else {
        /* Keyup always clears, even if flagged as repeat, to prevent sticking. */
        next = current & ~(uint32_t)button;
    }

    if (next != current)
        pad_keyboard_cache_publish_(state, next);
}

/* Reader side: only an acquire load of the snapshot; window_id is never read. */
static inline uint32_t pad_keyboard_cache_read(const PadKeyboardCache* state)
{
    if (state == NULL)
        return 0;
    return (uint32_t)atomic_load_explicit(&((PadKeyboardCache*)state)->snapshot,
                                          memory_order_acquire);
}

#endif /* FOLKLORE_PAD_KEYBOARD_CACHE_H */

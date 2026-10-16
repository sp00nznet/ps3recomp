#ifndef RSX_DRAW_BATCHES_H
#define RSX_DRAW_BATCHES_H

/*
 * Growable collector for RSX draw batch ranges (array or indexed). Owns only
 * its own storage. Ranges are appended in order, never coalesced and never
 * silently truncated. A zero-initialized list is valid and empty.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef RSX_DRAW_BATCH_REALLOC
#define RSX_DRAW_BATCH_REALLOC(ptr, size) realloc((ptr), (size))
#endif

#ifndef RSX_DRAW_BATCH_FREE
#define RSX_DRAW_BATCH_FREE(ptr) free(ptr)
#endif

#define RSX_DRAW_BATCH_INITIAL_CAPACITY 256u

typedef struct rsx_draw_batch {
    uint32_t first, count;
} rsx_draw_batch;

typedef struct rsx_draw_batch_list {
    rsx_draw_batch* data;
    uint32_t count, capacity;
} rsx_draw_batch_list;

static inline int rsx_draw_batch_valid_(const rsx_draw_batch_list* list)
{
    if (list->count > list->capacity)
        return 0;
    if ((list->data == NULL) != (list->capacity == 0u))
        return 0;
    return 1;
}

/* Grows capacity; on failure the list is left untouched. */
static inline int rsx_draw_batch_grow_(rsx_draw_batch_list* list)
{
    const size_t max_elems = SIZE_MAX / sizeof(rsx_draw_batch);
    uint32_t new_cap;
    void* p;

    if (list->count == UINT32_MAX)
        return 0;
    if (list->capacity == 0u)
        new_cap = RSX_DRAW_BATCH_INITIAL_CAPACITY;
    else if (list->capacity > UINT32_MAX / 2u)
        new_cap = UINT32_MAX;
    else
        new_cap = list->capacity * 2u;
    if (max_elems < (size_t)new_cap)
        new_cap = (uint32_t)max_elems;
    if (new_cap <= list->count)
        return 0;

    p = RSX_DRAW_BATCH_REALLOC(list->data, (size_t)new_cap * sizeof(rsx_draw_batch));
    if (p == NULL)
        return 0;
    list->data = (rsx_draw_batch*)p;
    list->capacity = new_cap;
    return 1;
}

/* Returns 1 on success, 0 on allocation/overflow/invalid state (list unchanged). */
static inline int rsx_draw_batch_push(rsx_draw_batch_list* list, uint32_t first, uint32_t count)
{
    if (list == NULL || !rsx_draw_batch_valid_(list))
        return 0;
    if (count == 0u)
        return 1;
    if ((uint64_t)first + (uint64_t)count - 1u > (uint64_t)UINT32_MAX)
        return 0;
    if (list->count == list->capacity && !rsx_draw_batch_grow_(list))
        return 0;
    if (list->count >= list->capacity || list->data == NULL)
        return 0;

    list->data[list->count].first = first;
    list->data[list->count].count = count;
    list->count++;
    return 1;
}

/* Drops all batches but keeps the allocation for reuse. */
static inline void rsx_draw_batch_clear(rsx_draw_batch_list* list)
{
    if (list != NULL)
        list->count = 0u;
}

/* Frees storage and zeroes the list; safe to call repeatedly. */
static inline void rsx_draw_batch_destroy(rsx_draw_batch_list* list)
{
    if (list == NULL)
        return;
    if (list->data != NULL)
        RSX_DRAW_BATCH_FREE(list->data);
    list->data = NULL;
    list->count = 0u;
    list->capacity = 0u;
}

#endif /* RSX_DRAW_BATCHES_H */

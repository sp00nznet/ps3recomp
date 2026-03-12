/*
 * ps3recomp - sys_interrupt HLE implementation
 *
 * Stub. Tags and interrupt threads are tracked but no real
 * interrupts fire. SPU interrupt use cases (SPURS, DMA completion)
 * are handled elsewhere.
 */

#include "sys_interrupt.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

typedef struct {
    int in_use;
    u32 classId;
} IntTag;

typedef struct {
    int in_use;
    sys_interrupt_tag_t tag;
    u64 intrthread;
    u64 arg;
} IntThread;

static IntTag s_tags[SYS_INTERRUPT_TAG_MAX];
static IntThread s_threads[SYS_INTERRUPT_THREAD_MAX];

/* Tags */

s32 sys_interrupt_tag_create(sys_interrupt_tag_t* tag, u32 intrtag, u32 classId)
{
    (void)intrtag;
    printf("[sys_interrupt] tag_create(class=%u)\n", classId);

    if (!tag) return (s32)CELL_INTERRUPT_ERROR_INVAL;

    for (int i = 0; i < SYS_INTERRUPT_TAG_MAX; i++) {
        if (!s_tags[i].in_use) {
            s_tags[i].in_use = 1;
            s_tags[i].classId = classId;
            *tag = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)CELL_INTERRUPT_ERROR_OVERFLOW;
}

s32 sys_interrupt_tag_destroy(sys_interrupt_tag_t tag)
{
    printf("[sys_interrupt] tag_destroy(%u)\n", tag);
    if (tag >= SYS_INTERRUPT_TAG_MAX || !s_tags[tag].in_use)
        return (s32)CELL_INTERRUPT_ERROR_NOT_FOUND;
    s_tags[tag].in_use = 0;
    return CELL_OK;
}

/* Interrupt threads */

s32 sys_interrupt_thread_establish(sys_interrupt_thread_handle_t* handle,
                                     sys_interrupt_tag_t tag,
                                     u64 intrthread, u64 arg)
{
    printf("[sys_interrupt] thread_establish(tag=%u)\n", tag);

    if (!handle) return (s32)CELL_INTERRUPT_ERROR_INVAL;
    if (tag >= SYS_INTERRUPT_TAG_MAX || !s_tags[tag].in_use)
        return (s32)CELL_INTERRUPT_ERROR_NOT_FOUND;

    for (int i = 0; i < SYS_INTERRUPT_THREAD_MAX; i++) {
        if (!s_threads[i].in_use) {
            s_threads[i].in_use = 1;
            s_threads[i].tag = tag;
            s_threads[i].intrthread = intrthread;
            s_threads[i].arg = arg;
            *handle = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)CELL_INTERRUPT_ERROR_OVERFLOW;
}

s32 sys_interrupt_thread_disestablish(sys_interrupt_thread_handle_t handle)
{
    printf("[sys_interrupt] thread_disestablish(%u)\n", handle);
    if (handle >= SYS_INTERRUPT_THREAD_MAX || !s_threads[handle].in_use)
        return (s32)CELL_INTERRUPT_ERROR_NOT_FOUND;
    s_threads[handle].in_use = 0;
    return CELL_OK;
}

s32 sys_interrupt_thread_eoi(void)
{
    /* End of interrupt — no-op in HLE */
    return CELL_OK;
}

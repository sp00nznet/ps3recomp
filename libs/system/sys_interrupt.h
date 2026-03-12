/*
 * ps3recomp - sys_interrupt HLE
 *
 * Kernel interrupt thread management: interrupt tags, handlers,
 * and interrupt thread creation. Stub — tags are tracked but
 * no real interrupts are delivered.
 */

#ifndef PS3RECOMP_SYS_INTERRUPT_H
#define PS3RECOMP_SYS_INTERRUPT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_INTERRUPT_ERROR_STAT        0x80010801
#define CELL_INTERRUPT_ERROR_NOSYS       0x80010802
#define CELL_INTERRUPT_ERROR_INVAL       0x80010803
#define CELL_INTERRUPT_ERROR_OVERFLOW    0x80010804
#define CELL_INTERRUPT_ERROR_NOT_FOUND   0x80010805

/* Constants */
#define SYS_INTERRUPT_TAG_MAX     32
#define SYS_INTERRUPT_THREAD_MAX  16

/* Types */
typedef u32 sys_interrupt_tag_t;
typedef u32 sys_interrupt_thread_handle_t;

typedef struct sys_interrupt_thread_attr {
    u32 priority;
    u32 stackSize;
    char name[32];
} sys_interrupt_thread_attr_t;

/* Functions */
s32 sys_interrupt_tag_create(sys_interrupt_tag_t* tag, u32 intrtag, u32 classId);
s32 sys_interrupt_tag_destroy(sys_interrupt_tag_t tag);

s32 sys_interrupt_thread_establish(sys_interrupt_thread_handle_t* handle,
                                     sys_interrupt_tag_t tag,
                                     u64 intrthread, u64 arg);
s32 sys_interrupt_thread_disestablish(sys_interrupt_thread_handle_t handle);

s32 sys_interrupt_thread_eoi(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SYS_INTERRUPT_H */

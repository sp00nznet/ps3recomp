/*
 * ps3recomp - LV2 syscall registration
 *
 * Calls all sys_X_init functions to populate the syscall dispatch table
 * with real HLE handlers.
 *
 * Registration order matters for conflicting syscall numbers:
 * timers are registered before events, so event handlers take precedence
 * for the colliding numbers (141, 142, 145). Timer sleep/time functions
 * remain available as direct C calls for the runtime to use.
 */

#include "lv2_syscall_table.h"
#include "sys_ppu_thread.h"
#include "sys_mutex.h"
#include "sys_cond.h"
#include "sys_semaphore.h"
#include "sys_rwlock.h"
#include "sys_event.h"
#include "sys_timer.h"
#include "sys_memory.h"
#include "sys_fs.h"

void lv2_register_all_syscalls(lv2_syscall_table* tbl)
{
    /* Initialize the table with unimplemented stubs first */
    lv2_syscall_table_init(tbl);

    /* Thread management */
    sys_ppu_thread_init(tbl);

    /* Synchronization primitives */
    sys_mutex_init(tbl);
    sys_cond_init(tbl);
    sys_semaphore_init(tbl);
    sys_rwlock_init(tbl);

    /* Timer and time (registered before events so event handlers
     * override the conflicting syscall numbers 141, 142, 145) */
    sys_timer_init(tbl);

    /* Event queues, ports, and flags */
    sys_event_init(tbl);

    /* Memory management */
    sys_memory_init(tbl);

    /* Filesystem */
    sys_fs_init(tbl);
}

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

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * TTY syscalls (used by PS3 CRT for debug output)
 *
 * sys_tty_read  (402) — read from TTY (stdin)
 * sys_tty_write (403) — write to TTY (stdout/stderr)
 *
 * These are among the most commonly called syscalls in CRT startup.
 * -----------------------------------------------------------------------*/

extern uint8_t* vm_base;

static int64_t sys_tty_write(ppu_context* ctx)
{
    /* s32 sys_tty_write(s32 ch, const void* buf, u32 len, u32* pwritelen) */
    uint32_t ch     = (uint32_t)ctx->gpr[3];
    uint32_t buf_ea = (uint32_t)ctx->gpr[4];
    uint32_t len    = (uint32_t)ctx->gpr[5];
    uint32_t pwr_ea = (uint32_t)ctx->gpr[6];

    (void)ch; /* channel number, ignored */

    if (buf_ea && len > 0 && vm_base) {
        /* Write guest string data to host stderr */
        fwrite(vm_base + buf_ea, 1, len, stderr);
        fflush(stderr);
    }

    /* Write back the number of bytes written */
    if (pwr_ea && vm_base) {
        uint32_t be_len = ((len >> 24) & 0xFF) | ((len >> 8) & 0xFF00) |
                          ((len << 8) & 0xFF0000) | ((len << 24) & 0xFF000000);
        memcpy(vm_base + pwr_ea, &be_len, 4);
    }

    return 0; /* CELL_OK */
}

static int64_t sys_tty_read(ppu_context* ctx)
{
    /* s32 sys_tty_read(s32 ch, void* buf, u32 len, u32* preadlen) */
    uint32_t prd_ea = (uint32_t)ctx->gpr[6];

    /* No TTY input available — return 0 bytes read */
    if (prd_ea && vm_base)
        memset(vm_base + prd_ea, 0, 4);

    return 0;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/

#define SYS_TTY_READ   402
#define SYS_TTY_WRITE  403

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

    /* TTY (debug console I/O — used by CRT startup) */
    lv2_syscall_register(tbl, SYS_TTY_READ,  sys_tty_read);
    lv2_syscall_register(tbl, SYS_TTY_WRITE, sys_tty_write);
}

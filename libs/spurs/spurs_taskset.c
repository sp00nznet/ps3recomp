/* spurs_taskset.c -- build the real CellSpursTaskset shared-memory layout (Option B,
 * phase B2). These functions write the BE structures that the reimplemented PM (B3)
 * and the real lifted leaf task read. ALL writes go through the byte-swapping
 * vm_write32/vm_write64 accessors (see the BIG-ENDIAN WARNING in spurs_taskset.h);
 * no native struct writes. Layout cross-referenced vs RPCS3 cellSpurs.h.
 *
 * Pure layout builders -- unit-tested in tests/test_spurs_taskset_main.c. cellSpurs.c
 * (the PPU create path) and the PM dispatcher call these.
 */
#include "spurs_taskset.h"
#include <stdint.h>

/* Initialize a fresh taskset: clear all task bitsets and write the header fields.
 * spurs_ea = the CellSpurs object EA; args = taskset args doubleword; wid = workload
 * id; size = taskset size; evf1/evf2 = attached event-flag ids (0 if none). */
void spurs_taskset_init(uint32_t taskset_ea, uint32_t spurs_ea, uint64_t args,
                        uint32_t wid, uint32_t size, uint32_t evf1, uint32_t evf2)
{
    /* clear running/ready/pending_ready/enabled/signalled/waiting (each 16 bytes) */
    for (uint32_t off = CSTS_RUNNING; off <= CSTS_WAITING; off += 0x10) {
        vm_write64(taskset_ea + off + 0, 0);
        vm_write64(taskset_ea + off + 8, 0);
    }
    vm_write64(taskset_ea + CSTS_SPURS, (uint64_t)spurs_ea); /* CellSpurs* (be u64) */
    vm_write64(taskset_ea + CSTS_ARGS,  args);
    vm_write32(taskset_ea + CSTS_WID,   wid);
    vm_write64(taskset_ea + CSTS_X78,   0);                  /* on-exit handler EA */
    vm_write32(taskset_ea + CSTS_SIZE_FIELD,     size);
    vm_write32(taskset_ea + CSTS_EVENT_FLAG_ID1, evf1);
    vm_write32(taskset_ea + CSTS_EVENT_FLAG_ID2, evf2);
}

/* Set or clear one task's bit in a taskset bitset (ready/pending/enabled at +0x10/+0x20/+0x30) from a host thread.
 *
 * These bitsets share the taskset's first 128-byte line with the SPU task library, which updates them with
 * GETLLAR/PUTLLC, and with the host's task exit (spu_taskset_task_exited). spurs_bitset_set/_clear read the word
 * with vm_read32 outside the line's own lock and write it back with vm_write32, so a change committed between the
 * two was undone. Do the read-modify-write on the raw guest bytes under that line's lock (vm_write32 takes the
 * same lock itself when the line is reserved, and it does not nest), then bump the version and drop every
 * reservation on the line (notify takes the table lock itself, so it runs after the line lock is dropped). */
void spurs_taskset_bitset_update(uint32_t bitset_ea, uint32_t taskId, int set)
{
    extern void spu_linelock_lock(uint32_t); extern void spu_linelock_unlock(uint32_t);
    extern void spu_linelock_release(uint32_t); extern int spu_coh_is_reserved(uint32_t);
    extern void spu_coh_notify_write(uint32_t);
    extern uint8_t* vm_base;
    uint8_t* p = vm_base + bitset_ea + taskId / 8;
    const uint8_t m = (uint8_t)(0x80u >> (taskId % 8));
    spu_linelock_lock(bitset_ea + taskId / 8);
    if (set) *p |= m; else *p &= (uint8_t)~m;
    spu_linelock_release(bitset_ea + taskId / 8);
    if (spu_coh_is_reserved(bitset_ea)) spu_coh_notify_write(bitset_ea);
}

/* Take the lowest free task id of a taskset: find the first clear bit of the enabled bitset and set it, as one
 * step under the lock-line lock, so a task exit clearing its bit at the same time is never undone and two callers
 * never get the same id. Returns `max_tasks` when every id is taken. */
uint32_t spurs_taskset_take_slot(uint32_t taskset_ea, uint32_t max_tasks)
{
    extern void spu_linelock_lock(uint32_t); extern void spu_linelock_unlock(uint32_t);
    extern void spu_linelock_release(uint32_t); extern int spu_coh_is_reserved(uint32_t);
    extern void spu_coh_notify_write(uint32_t);
    extern uint8_t* vm_base;
    uint8_t* en = vm_base + taskset_ea + CSTS_ENABLED;
    uint32_t slot = max_tasks;
    spu_linelock_lock(taskset_ea + CSTS_ENABLED);
    for (uint32_t t = 0; t < max_tasks; t++) {
        const uint8_t m = (uint8_t)(0x80u >> (t % 8));
        if (!(en[t / 8] & m)) { en[t / 8] |= m; slot = t; break; }
    }
    if (slot < max_tasks) {
        spu_linelock_release(taskset_ea + CSTS_ENABLED);
        if (spu_coh_is_reserved(taskset_ea + CSTS_ENABLED))
            spu_coh_notify_write(taskset_ea + CSTS_ENABLED);
    } else {
        spu_linelock_unlock(taskset_ea + CSTS_ENABLED);
    }
    return slot;
}

/* Register a task: write task_info[taskId] (args/elf/context/ls_pattern) and mark the
 * task enabled + ready so the PM's SELECT_TASK picks it. arg/ls_pattern may be NULL. */
void spurs_taskset_add_task(uint32_t taskset_ea, uint32_t taskId, uint64_t elf_ea,
                            uint64_t context, const uint32_t arg[4],
                            const uint32_t ls_pattern[4])
{
    uint32_t ti = spurs_taskset_taskinfo_ea(taskset_ea, taskId);
    for (int i = 0; i < 4; i++) vm_write32(ti + TI_ARGS + i * 4, arg ? arg[i] : 0);
    vm_write64(ti + TI_ELF, elf_ea);
    vm_write64(ti + TI_CONTEXT, context);
    for (int i = 0; i < 4; i++)
        vm_write32(ti + TI_LS_PATTERN + i * 4, ls_pattern ? ls_pattern[i] : 0);
    spurs_taskset_bitset_update(taskset_ea + CSTS_ENABLED, taskId, 1);
    spurs_taskset_bitset_update(taskset_ea + CSTS_READY,   taskId, 1);
}

/* Set the taskset's on-task-exit handler EA (CellSpursTaskset.x78). */
void spurs_taskset_set_exit_handler(uint32_t taskset_ea, uint64_t handler_ea)
{
    vm_write64(taskset_ea + CSTS_X78, handler_ea);
}

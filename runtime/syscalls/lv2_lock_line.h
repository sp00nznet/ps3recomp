/*
 * ps3recomp - sys_process_is_spu_lock_line_reservation_address (14)
 *
 * The address/flags contract of syscall 14, split out of lv2_register.c so
 * it can be unit-tested without linking the whole syscall layer
 * (see tests/syscalls).
 */

#ifndef LV2_LOCK_LINE_H
#define LV2_LOCK_LINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Answer whether SPUs may place lock-line reservations (GETLLAR/PUTLLC) on
 * the guest address `addr` with the access-right `flags` (bit 0 = RawSPU,
 * bit 1 = SPU threads). Returns 0 when reservations are allowed, or the
 * negative-encoded CELL error the syscall must return (CELL_EPERM /
 * CELL_EINVAL, in this runtime's (int64_t)(int32_t) convention). The
 * contract is RPCS3's process_is_spu_lock_line_reservation_address
 * (Emu/Cell/lv2/sys_process.cpp). */
int64_t lv2_spu_lock_line_classify(uint32_t addr, uint64_t flags);

#ifdef __cplusplus
}
#endif

#endif /* LV2_LOCK_LINE_H */

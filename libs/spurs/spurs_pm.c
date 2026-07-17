/* spurs_pm.c -- SPURS taskset Policy Module logic (Option B, phase B3), clean-room
 * reimplementation of the parts of RPCS3 cellSpursSpu.cpp our HLE needs. Operates on
 * the BE shared-memory layout (spurs_taskset.h); all guest access via the vm_read /
 * vm_write byte-swapping accessors.
 *
 * This module is the "SPU kernel" that schedules our real lifted leaf task: it selects a
 * ready task and builds the SpursTasksetContext at LS 0x2700, then the dispatcher loads
 * the task's context and runs it. Selection logic cross-referenced vs RPCS3
 * spursTasksetProcessRequest(SELECT_TASK): readyButNotRunning = ready & ~running, scanned
 * round-robin starting after last_scheduled_task; task N occupies bitset bit (127-N).
 */
#include "spurs_taskset.h"
#include <stdint.h>

/* Select the next task to run: first task that is ready and not running, scanning
 * round-robin from (last_scheduled_task + 1). Returns the taskId (0..127) or -1 if none
 * is runnable. (The wkl_flag_wait_task exclusion is a refinement we add when needed.) */
int spurs_pm_select_task(uint32_t taskset_ea, uint32_t last_scheduled_task)
{
    uint32_t ready_ea   = taskset_ea + CSTS_READY;
    uint32_t running_ea = taskset_ea + CSTS_RUNNING;
    for (uint32_t i = 1; i <= 128; i++) {
        uint32_t t = (last_scheduled_task + i) & 127;     /* round-robin, wrap at 128 */
        if (spurs_bitset_test(ready_ea, t) && !spurs_bitset_test(running_ea, t))
            return (int)t;
    }
    return -1;
}

/* Mark a task as the running one: clear ready, set running, record last_scheduled_task.
 * (RPCS3 moves the task ready->running on dispatch.) last_scheduled_task is the byte at
 * CSTS_LAST_SCHEDULED (0x73) = the low byte of the big-endian word at 0x70. */
void spurs_pm_mark_running(uint32_t taskset_ea, uint32_t taskId)
{
    spurs_bitset_clear(taskset_ea + CSTS_READY, taskId);
    spurs_bitset_set(taskset_ea + CSTS_RUNNING, taskId);
    uint32_t w = vm_read32(taskset_ea + 0x70);
    w = (w & 0xFFFFFF00u) | (taskId & 0xFFu);
    vm_write32(taskset_ea + 0x70, w);
}

/* Build the SpursTasksetContext at LS 0x2700 for the selected task, and DMA its TaskInfo
 * into LS 0x2780. ls = the SPU local store (256 KB). Returns the task ELF EA (low 3 bits
 * are flags, masked off for loading). */
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t taskset_ea, uint32_t taskId,
                                uint32_t spuNum, uint32_t dmaTagId)
{
    /* SpursTasksetContext header fields (LS-local, written big-endian like guest mem). */
    /* taskset ptr @0x27B8 (be u64), spuNum @0x27CC, dmaTagId @0x27D0, taskId @0x27D4,
     * tasksetMgmtAddr @0x2FB8, x2FC0 cleared. We write directly into ls[] big-endian. */
    #define LS_BE32(off, v) do { uint32_t _v=(v); ls[(off)+0]=(uint8_t)(_v>>24); ls[(off)+1]=(uint8_t)(_v>>16); \
                                 ls[(off)+2]=(uint8_t)(_v>>8); ls[(off)+3]=(uint8_t)_v; } while(0)
    #define LS_BE64(off, v) do { uint64_t _w=(v); LS_BE32((off), (uint32_t)(_w>>32)); LS_BE32((off)+4,(uint32_t)_w); } while(0)

    /* Copy the CellSpursTaskset HEADER (first 0x80 bytes: the 6 bitsets, spurs@0x60,
     * args@0x68, wid@0x74, x78@0x78) from the main-memory taskset into LS 0x2700. RPCS3
     * spursTasksetStartTask reads taskset->spurs / taskset->args from LS 0x2700+0x60/0x68
     * to build the leaf's r4 = {args (d0), spurs EA (d1)}; without this the leaf gets a
     * garbage SPURS base and DMAs from a bad address. The STC fields written below sit at
     * offsets >= 0x80 (TaskInfo array region in the union view) so they don't overlap. */
    for (int o = 0; o < 0x80; o += 4)
        LS_BE32(STC_BASE + o, vm_read32(taskset_ea + o));

    LS_BE64(STC_TASKSET_PTR,  (uint64_t)taskset_ea);
    LS_BE32(STC_SPU_NUM,      spuNum);
    LS_BE32(STC_DMA_TAG_ID,   dmaTagId);
    LS_BE32(STC_TASK_ID,      taskId);
    LS_BE32(STC_TASKSET_MGMT_ADDR, STC_BASE);
    LS_BE64(STC_X2FC0, 0);
    /* Task-syscall path: a SPURS task reads syscallAddr from its context and branches to
     * it (e.g. to EXIT). The real kernel sets it to the PM's in-LS syscall entry (0xA70);
     * we don't have the PM resident, so we set the same address and INTERCEPT a branch to
     * it in spu_indirect_branch (spu_channels.c) to HLE the syscall. Without this the task
     * branches to 0 -> null call -> halt (image=7's NULL_POINTER cascade). kernelMgmtAddr
     * -> the SPURS kernel context (LS 0x100). */
    LS_BE32(STC_KERNEL_MGMT_ADDR, 0x100);
    LS_BE32(STC_SYSCALL_ADDR,     CELL_SPURS_TASKSET_PM_SYSCALL_ADDR);

    /* SPURS KERNEL context (LS 0x100..0x1FF): the SPU task library's fast-path
     * validation reads fields the real kernel + taskset PM populate there.
     * moduleId @0x1E4 must be "TK" (stamped by the taskset PM, RPCS3
     * spursTasksetInit) -- without it every task-API wait (event flag / queue)
     * fails its context check with ERROR_STAT 0x80410909: LBP's binkspu movie
     * IO task spun forever on exactly that, so BinkWait never completed and
     * the intro never produced a frame. Also plant the spurs instance ptr
     * (@0x1C0, read from the taskset header) + spuNum/dmaTagId, which the
     * library reads next to the moduleId. */
    {
        uint32_t spurs_lo = vm_read32(taskset_ea + 0x64);   /* be64 spurs @0x60 */
        LS_BE64(0x1C0, (uint64_t)spurs_lo);
        LS_BE32(0x1C8, spuNum);
        LS_BE32(0x1CC, dmaTagId);
        /* The task library's fast-path check FSMs on the halfword at 0x1E8
         * (ceqh of rotqbyi(LS[0x1E0],6), select-mask bit -> word0); stamp the
         * neighboring slots too -- they are all inside the moduleId/pad area
         * the real PM owns and nothing else reads them. */
        ls[0x1E4] = 'T'; ls[0x1E5] = 'K';
        ls[0x1E6] = 'T'; ls[0x1E7] = 'K';
        ls[0x1E8] = 'T'; ls[0x1E9] = 'K';
    }

    /* DMA the selected task's TaskInfo (48 bytes) into LS 0x2780 (the kernel temp area).
     * Read each word BE and re-store BE -> the raw bytes are preserved verbatim. */
    uint32_t ti = spurs_taskset_taskinfo_ea(taskset_ea, taskId);
    for (int o = 0; o < TI_SIZE; o += 4)
        LS_BE32(STC_TEMP_TASKINFO + o, vm_read32(ti + o));

    return vm_read64(ti + TI_ELF);       /* caller masks elf & ~7 to load */
    #undef LS_BE32
    #undef LS_BE64
}

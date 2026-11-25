/*
 * test_spursjq_guest.c -- cellSpursJq attributes, handles and ports on guest memory.
 *
 * The job-queue attribute is stored big-endian (maxJobs 64, maxGrab 1),
 * Create stores the queue's slot as a BE handle, Port2Create takes
 * (port, queue) in the SDK's order and copies that handle, a synchronous
 * push returns a BE job id the queue can wait on, and GetCount writes BE.
 * Errors are libspurs_jq's (0x80410A00 | errno). The SPU job runner is
 * stubbed: this checks the PPU-side ABI only.
 * Run: python tools/test_spursjq_guest.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ppu/ppu_memory.h"
#include "cellSpursJq.h"
#include "spu/spu_workload.h"
#include "spu/spu_interp.h"

static uint8_t s_guest_mem[0x10000];
uint8_t* vm_base = s_guest_mem;

/* Runtime hooks the vm_write* macros reference; none may fire here. */
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
int g_resv_store_active;
void ppu_resv_break_store(uint64_t addr) { (void)addr; abort(); }
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t addr) { (void)addr; abort(); }
void spu_lockline_lock(void) { abort(); }
void spu_lockline_unlock(void) { abort(); }
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(void*)) { (void)nid; (void)name; (void)fn; }

/* The SPU side: jobs are not run here. */
uint32_t spu_interp_run_until(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa) { (void)ctx; (void)start_lsa; (void)stop_lsa; return 0; }
int spu_run_spurs_job(spu_lifted_entry_fn entry, int image_id, uint32_t job_ea, uint32_t job_desc_size) { (void)entry; (void)image_id; (void)job_ea; (void)job_desc_size; return 0; }
int spu_workload_dispatch_job(const uint8_t* image, uint32_t image_size, uint32_t job_ea, uint32_t job_desc_size) { (void)image; (void)image_size; (void)job_ea; (void)job_desc_size; return 0; }

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); fails++; } \
} while (0)

static void test_spursjq(void)
{
    const u32 jqattr_ea = 0x3E00;
    const u32 jq0_ea    = 0x3E40;
    const u32 jq1_ea    = 0x3E80;
    const u32 port1_ea  = 0x3F00; /* 128-byte aligned */
    const u32 jid_ea    = 0x3F80;
    const u32 jcnt_ea   = 0x3F84;

    CHECK(cellSpursJobQueueAttributeInitialize(NULL) == (s32)CELL_SPURS_JQ_ERROR_NULL_POINTER,
          "cellSpursJobQueueAttributeInitialize(NULL) == 0x80410A11");
    CHECK(cellSpursJobQueueAttributeInitialize((CellSpursJobQueueAttribute*)(uintptr_t)jqattr_ea) == CELL_OK,
          "cellSpursJobQueueAttributeInitialize succeeds");
    CHECK(vm_read32(jqattr_ea + 0) == 64 && vm_read32(jqattr_ea + 4) == 1,
          "CellSpursJobQueueAttribute writes BE maxJobs=64, maxGrab=1");
    CHECK(cellSpursJobQueueAttributeSetMaxGrab((CellSpursJobQueueAttribute*)(uintptr_t)jqattr_ea, 0) == (s32)CELL_SPURS_JQ_ERROR_INVAL,
          "cellSpursJobQueueAttributeSetMaxGrab(0) == 0x80410A02");
    CHECK(cellSpursJobQueueAttributeSetMaxGrab((CellSpursJobQueueAttribute*)(uintptr_t)jqattr_ea, 4) == CELL_OK &&
          vm_read32(jqattr_ea + 4) == 4, "SetMaxGrab(4) writes BE 4");
    CHECK(cellSpursJobQueuePort2Create(NULL, NULL) == (s32)CELL_SPURS_JQ_ERROR_NULL_POINTER,
          "cellSpursJobQueuePort2Create(NULL,NULL) == 0x80410A11");

    CHECK(cellSpursCreateJobQueue(NULL, (CellSpursJobQueue*)(uintptr_t)jq0_ea,
                                  (const CellSpursJobQueueAttribute*)(uintptr_t)jqattr_ea, NULL, 0) == CELL_OK,
          "CreateJobQueue slot 0 succeeds");
    CHECK(cellSpursCreateJobQueue(NULL, (CellSpursJobQueue*)(uintptr_t)jq1_ea,
                                  (const CellSpursJobQueueAttribute*)(uintptr_t)jqattr_ea, NULL, 0) == CELL_OK,
          "CreateJobQueue slot 1 succeeds");
    CHECK(vm_read32(jq1_ea) == 1u, "CreateJobQueue stores BE slot 1 (consistent with _cellSpursCreateJobQueue)");
    CHECK(cellSpursJobQueuePort2Create((CellSpursJobQueuePort*)(uintptr_t)port1_ea,
                                       (CellSpursJobQueue*)(uintptr_t)jq1_ea) == CELL_OK &&
          vm_read32(port1_ea) == 1u, "Port2Create copies BE slot 1");
    CHECK(cellSpursJobQueuePort2PushSync((CellSpursJobQueuePort*)(uintptr_t)port1_ea,
                                         NULL, 0, 0, (CellSpursJobId*)(uintptr_t)jid_ea) == CELL_OK,
          "Port2PushSync on slot 1 succeeds");
    u32 jid = vm_read32(jid_ea);
    CHECK(jid == 1u, "Port2PushSync writes BE jobId == 1");
    CHECK(cellSpursJobQueueWait((CellSpursJobQueue*)(uintptr_t)jq1_ea, jid) == CELL_OK,
          "JobQueueWait on slot 1 succeeds");
    CHECK(cellSpursJobQueueGetCount((CellSpursJobQueue*)(uintptr_t)jq1_ea, (u32*)(uintptr_t)jcnt_ea) == CELL_OK &&
          vm_read32(jcnt_ea) == 0u, "JobQueueGetCount writes BE 0");
    CHECK(cellSpursDestroyJobQueue((CellSpursJobQueue*)(uintptr_t)jq0_ea) == CELL_OK, "DestroyJobQueue slot 0");
    CHECK(cellSpursDestroyJobQueue((CellSpursJobQueue*)(uintptr_t)jq1_ea) == CELL_OK, "DestroyJobQueue slot 1");
}

int main(void)
{
    test_spursjq();
    if (fails) {
        fprintf(stderr, "FAILED: %d check(s)\n", fails);
        return 1;
    }
    puts("PASS: cellSpursJq attributes, handles and ports");
    return 0;
}

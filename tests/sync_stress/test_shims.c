/*
 * Standalone definitions for services that the sync sources call through
 * optional title/runtime integration hooks. The stress suite tests the
 * primitives directly, without linking a generated title.
 */
#include "../../runtime/platform/win32_compat.h"   /* QueryPerformanceCounter on every host */

#include <stdint.h>

extern uint8_t* vm_base;

int64_t lv2_usec_deadline(uint64_t usec)
{
    LARGE_INTEGER now;
    LARGE_INTEGER freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return now.QuadPart + (int64_t)((usec * (uint64_t)freq.QuadPart) / 1000000ull);
}

int lv2_deadline_passed(int64_t deadline)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart >= deadline;
}

void ydkj_release_pending_threads(void) {}

uint32_t vm_read32(uint64_t addr)
{
    const uint8_t* p = vm_base + (uint32_t)addr;
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

/* Diagnostic/port hooks sys_semaphore.c calls; real definitions live in the
 * per-title host code (lbp/main.cpp, runtime/ppu/ppu_loader.cpp). */
void lbp_breadcrumb_dump(const char* tag) { (void)tag; }
void ps3_spu_job_complete_pending(void) {}
void ppu_log_host_chain(const char* tag) { (void)tag; }

int spu_dispatch_frame_by_queue(uint32_t comp_queue, uint32_t work_ea)
{
    (void)comp_queue;
    (void)work_ea;
    return 0;
}

/* The SPU-side event handoff sys_event_port_send writes when a port is bound
 * to an SPU thread. Defined by runtime/spu/spu_interp.c, which this suite
 * does not link; nothing here binds a port to an SPU, so the values are
 * never read back. */
uint32_t g_spu_pending_evt[3];
int      g_spu_pending_evt_valid;

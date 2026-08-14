/*
 * Standalone definitions for services that the sync sources call through
 * optional title/runtime integration hooks. The stress suite tests the
 * primitives directly, without linking a generated title.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
void lbp_hle_complete_pending(void) {}
void ppu_log_host_chain(const char* tag) { (void)tag; }

int spu_dispatch_frame_by_queue(uint32_t comp_queue, uint32_t work_ea)
{
    (void)comp_queue;
    (void)work_ea;
    return 0;
}

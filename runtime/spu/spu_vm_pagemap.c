/* Committed-page bitmap: one bit per 64 KB page across the 4 GB guest space
 * (8 KB total). The MFC DMA guard (mfc_ea_range_committed in spu_dma.h) tests
 * these bits instead of calling VirtualQuery per transfer -- the syscall version
 * measured 94 CPU-s in a 50 s run.
 *
 * Storage lives here, in the runtime, because spu_dma.h is a header inlined into
 * whichever TU handles MFC (the runtime's spu_channels.c, or a per-game override
 * of it). It used to be defined in lbp/main.cpp, which made every other port fail
 * to link. Titles whose demand-commit fault handler seeds the bitmap just declare
 * `extern uint8_t g_vm_page_bitmap[65536/8]` and set bits; the guard treats it as
 * a self-healing cache (miss -> one VirtualQuery -> remember), so a port that
 * never seeds it is correct, just slower on first touch per page. */

unsigned char g_vm_page_bitmap[65536 / 8];

/* Strict MFC verified-page cache (mfc_strict_guest_span in spu_dma.h).
 * Strict DMA validates every touched host page with VirtualQuery (Windows) or
 * a /proc/self/maps scan (Linux). Profiling Folklore gameplay on Windows put
 * that query at ~47% of all process CPU; on Linux each transfer reparses maps.
 * A host whose guest pages, once committed read-write, are never decommitted,
 * released or reduced in protection may opt in; a 64 KB guest page is then
 * validated once by the same strict check and remembered. Pages that are not
 * wholly committed read-write are never remembered and keep the exact,
 * uncached per-transfer check. The same opt-in skips revalidating an SPU's
 * own context-embedded local store, which the bounds check already covers. */
#if defined(_MSC_VER)
#include <intrin.h>
#endif
volatile int g_mfc_strict_page_cache;
volatile unsigned char g_mfc_strict_verified[65536 / 8];

void mfc_strict_enable_verified_page_cache(void) { g_mfc_strict_page_cache = 1; }

void mfc_strict_note_verified_page(unsigned int page)
{
#if defined(_MSC_VER)
    _InterlockedOr8((volatile char*)&g_mfc_strict_verified[(page >> 3) & 8191u], (char)(1u << (page & 7)));
#else
    __atomic_fetch_or(&g_mfc_strict_verified[(page >> 3) & 8191u], (unsigned char)(1u << (page & 7)), __ATOMIC_RELAXED);
#endif
}

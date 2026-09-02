#include "ps3emu/yz_frontier_trace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <stdatomic.h>
#include <time.h>
#endif

#define YZ_FRONTIER_CAPACITY 131072u
#define YZ_FRONTIER_PATH_MAX 1024u

typedef struct yz_frontier_record {
    uint64_t sequence;  /* published last; zero means incomplete */
    uint64_t qpc;
    uint32_t type;
    uint32_t actor;
    uint32_t pc;
    uint32_t arg[6];
} yz_frontier_record;

typedef struct yz_frontier_file_header {
    char magic[8];
    uint32_t version;
    uint32_t record_size;
    uint32_t capacity;
    uint32_t reason;
    uint64_t write_count;
    uint64_t retained_begin;
    uint64_t qpc_frequency;
} yz_frontier_file_header;

static yz_frontier_record s_records[YZ_FRONTIER_CAPACITY];
static char s_dump_prefix[YZ_FRONTIER_PATH_MAX] = "scratch/frontier_ring";
static uint64_t s_qpc_frequency;
/* Mode '2' (armed at init): early-boot quiet phases legitimately trip the
 * stall triggers (boot 49: the RSX poller's 5 s window fired during a
 * disc-bound load at 2,676 records and the one-shot spent + disarmed the
 * ring before the real wedge). In that mode dumps are numbered, capped, and
 * the ring RE-ARMS after each so the terminal-stall dump always exists. */
#define YZ_FRONTIER_MAX_DUMPS 8
static int s_rearm_after_dump;
/* Boots 49-51: dump I/O inside the bootstrap window (3.7MB bin + a 64k-line
 * formatted tsv, twice, at ~8s) is the leading rate suspect for the frame-52
 * early freeze (2/3). Stall dumps now wait for real progress; the tsv is
 * opt-in (YZ_FRONTIER_RING_TSV=1) — the .bin is complete and ring_decode.py
 * reads it directly. */
#define YZ_FRONTIER_PROGRESS_GATE 300u
static volatile uint32_t s_progress;
static int s_tsv_enabled;
/* Dialogue-transition diagnostics need minutes of task/event history, while
 * FIFO park and SPU halt sampling can consume the 64k ring in seconds.  Keep
 * the default recorder unchanged; this opt-in compact mode drops only those
 * two high-rate scheduling samples. */
static int s_compact;
/* Focused animation/render ping-pong diagnostic.  When selected, the shared
 * recorder retains only the three low-rate parity event families below.  It
 * avoids letting unrelated syscall/FIFO/SPURS traffic evict the correlation
 * window and keeps the hot path to one predicted branch for other events. */
static int s_parity_only;
/* Mode '3' (boots 49-53: 0/5 early-boot survival with the ring armed at
 * init vs 5/5 without the ring era — the armed emit traffic in the
 * bootstrap window is the correlated suspect): keep the recorder dormant
 * until real progress, then arm. Bootstrap emits cost one predicted
 * branch; full evidence from frame 300 on (the dialogue boundary is
 * ~3300+). */
static int s_arm_on_progress;

#ifdef _WIN32
static volatile LONG64 s_write_count;
static volatile LONG s_enabled;
static volatile LONG s_armed;
static volatile LONG s_dumped;

static uint64_t yz_frontier_now(void)
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
}

static uint64_t yz_frontier_next(void)
{
    return (uint64_t)InterlockedIncrement64(&s_write_count) - 1u;
}

static uint64_t yz_frontier_count(void)
{
    return (uint64_t)InterlockedCompareExchange64(&s_write_count, 0, 0);
}

static void yz_frontier_publish(volatile uint64_t* sequence, uint64_t value)
{
    InterlockedExchange64((volatile LONG64*)sequence, (LONG64)value);
}
#else
static _Atomic uint64_t s_write_count;
static _Atomic int s_enabled;
static _Atomic int s_armed;
static _Atomic int s_dumped;

static uint64_t yz_frontier_now(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

static uint64_t yz_frontier_next(void)
{
    return atomic_fetch_add_explicit(&s_write_count, 1u, memory_order_relaxed);
}

static uint64_t yz_frontier_count(void)
{
    return atomic_load_explicit(&s_write_count, memory_order_acquire);
}

static void yz_frontier_publish(volatile uint64_t* sequence, uint64_t value)
{
    atomic_store_explicit((_Atomic uint64_t*)sequence, value,
                          memory_order_release);
}
#endif

static const char* yz_frontier_event_name(uint32_t type)
{
    switch (type) {
    case YZ_FT_ARM:              return "arm";
    case YZ_FT_PPU_SITE:         return "ppu-site";
    case YZ_FT_PPU_VALUE:        return "ppu-value";
    case YZ_FT_PPU_STORE:        return "ppu-store";
    case YZ_FT_SPU_JOB_SELECT:   return "spu-job-select";
    case YZ_FT_SPU_DMA_CMD:      return "spu-dma";
    case YZ_FT_SPU_OUT_MBOX:     return "spu-out-mbox";
    case YZ_FT_SPU_INTR_MBOX:    return "spu-intr-mbox";
    case YZ_FT_EVENT_WAIT:       return "event-wait";
    case YZ_FT_EVENT_SET:        return "event-set";
    case YZ_FT_EVENT_QUEUE:      return "event-queue";
    case YZ_FT_SPU_HALT:         return "spu-halt";
    case YZ_FT_RSX_STATE:        return "rsx-state";
    case YZ_FT_STALL:            return "stall";
    case YZ_FT_FIFO_PARK:        return "fifo-park";
    case YZ_FT_FIFO_STATE:       return "fifo-state";
    case YZ_FT_FIFO_PUBLICATION: return "fifo-publication";
    case YZ_FT_SPU_STATE:        return "spu-state";
    case YZ_FT_SPU_JOB_STATE:    return "spu-job-state";
    case YZ_FT_SPU_MFC_STATE:    return "spu-mfc-state";
    case YZ_FT_EVENT_STATE:      return "event-state";
    case YZ_FT_TASK_WAIT:        return "task-wait";
    case YZ_FT_TASK_SIGNAL:      return "task-signal";
    case YZ_FT_ATOMIC_COMMIT:    return "atomic-commit";
    case YZ_FT_SYSCALL:          return "syscall";
    case YZ_FT_PPU_THREAD:       return "ppu-thread";
    case YZ_FT_PPU_STACK:        return "ppu-stack";
    case YZ_FT_SPURS_WORKLOAD:   return "spurs-workload";
    case YZ_FT_SPURS_TASKSET:    return "spurs-taskset";
    case YZ_FT_SPURS_TASK:       return "spurs-task";
    case YZ_FT_JOBCHAIN:         return "jobchain";
    case YZ_FT_JOB:              return "job";
    case YZ_FT_COMPLETION:       return "completion";
    case YZ_FT_RESERVATION:      return "reservation";
    case YZ_FT_RELEASE_JOURNAL:  return "release-journal";
    case YZ_FT_FRAME_CREDIT:     return "frame-credit";
    case YZ_FT_GCM_CALLBACK:     return "gcm-callback";
    case YZ_FT_AUTOPSY:          return "autopsy";
    case YZ_FT_LOGICAL_STATE:    return "logical-state";
    case YZ_FT_SYNC_STATE:       return "sync-state";
    case YZ_FT_PARITY_FLIP:      return "parity-flip";
    case YZ_FT_PARITY_MOTION:    return "parity-motion";
    case YZ_FT_PARITY_MOTION_DMA:return "parity-motion-dma";
    case YZ_FT_PARITY_SURFACE:   return "parity-surface";
    case YZ_FT_PARITY_SURFACE_AUX:return "parity-surface-aux";
    case YZ_FT_PARITY_RENDER:    return "parity-render";
    case YZ_FT_PARITY_GUEST_BLIT:return "parity-guest-blit";
    case YZ_FT_PARITY_CB_RECYCLE:return "parity-cb-recycle";
    default:                     return "unknown";
    }
}

int yz_frontier_trace_init(void)
{
    const char* enabled = getenv("YZ_FRONTIER_RING");
    const char* path = getenv("YZ_FRONTIER_RING_PATH");
    const char* compact = getenv("YZ_FRONTIER_RING_COMPACT");
    const char* parity = getenv("YZ_FRONTIER_RING_PARITY");
    if (!enabled || !*enabled || *enabled == '0')
        return 0;

    if (path && *path) {
        snprintf(s_dump_prefix, sizeof(s_dump_prefix), "%s", path);
        s_dump_prefix[sizeof(s_dump_prefix) - 1u] = '\0';
    }
    s_compact = compact && *compact == '1';
    s_parity_only = parity && *parity == '1';
#ifdef _WIN32
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        s_qpc_frequency = (uint64_t)frequency.QuadPart;
        InterlockedExchange(&s_enabled, 1);
    }
#else
    s_qpc_frequency = 1000000000ull;
    atomic_store_explicit(&s_enabled, 1, memory_order_release);
#endif
    fprintf(stderr,
            "[frontier-ring] configured dormant capacity=%u bytes=%zu compact=%d parity=%d\n",
            YZ_FRONTIER_CAPACITY, sizeof(s_records), s_compact,
            s_parity_only);
    fflush(stderr);
    /* YZ_FRONTIER_RING=2 (2026-08-06 handoff-ordering frontier): arm at init
     * instead of waiting for the semantic Job B selection. The ring is a
     * wrap-around recorder, so an early arm still retains the newest 64k
     * events at dump time — exactly the pre-stall tail the race decode needs. */
    if (*enabled == '2' || *enabled == '3') {
        const char* tsv = getenv("YZ_FRONTIER_RING_TSV");
        s_tsv_enabled = (tsv && *tsv == '1') ? 1 : 0;
        s_rearm_after_dump = 1;
        if (*enabled == '2') {
            yz_frontier_trace_arm(0xFFFFFFFFu, 0, 0, 0, 0, 0);
            fprintf(stderr, "[frontier-ring] armed at init "
                    "(YZ_FRONTIER_RING=2, numbered dumps, re-arm, cap %d, "
                    "stall dumps gated until frame %u, tsv=%d)\n",
                    YZ_FRONTIER_MAX_DUMPS, YZ_FRONTIER_PROGRESS_GATE,
                    s_tsv_enabled);
        } else {
            s_arm_on_progress = 1;
            fprintf(stderr, "[frontier-ring] dormant until frame %u "
                    "(YZ_FRONTIER_RING=3; bootstrap emits disabled — the "
                    "armed-at-init ring correlated 5/5 with the early "
                    "freeze, boots 49-53)\n", YZ_FRONTIER_PROGRESS_GATE);
        }
        fflush(stderr);
    }
    return 1;
}

void yz_frontier_trace_progress(uint32_t frame)
{
    if (frame > s_progress)
        s_progress = frame;
    if (s_arm_on_progress && frame >= YZ_FRONTIER_PROGRESS_GATE &&
        !yz_frontier_trace_is_armed()) {
        yz_frontier_trace_arm(0xFFFFFFFFu, 0, 0, 0, 0, 0);
        fprintf(stderr, "[frontier-ring] armed at frame %u "
                "(mode 3 progress arm)\n", frame);
        fflush(stderr);
    }
}

int yz_frontier_trace_enabled(void)
{
#ifdef _WIN32
    return InterlockedCompareExchange(&s_enabled, 0, 0) != 0;
#else
    return atomic_load_explicit(&s_enabled, memory_order_acquire) != 0;
#endif
}

int yz_frontier_trace_is_armed(void)
{
#ifdef _WIN32
    return InterlockedCompareExchange(&s_armed, 0, 0) != 0;
#else
    return atomic_load_explicit(&s_armed, memory_order_acquire) != 0;
#endif
}

void yz_frontier_trace_emit(uint32_t type, uint32_t actor, uint32_t pc,
                            uint32_t a0, uint32_t a1, uint32_t a2,
                            uint32_t a3, uint32_t a4, uint32_t a5)
{
    yz_frontier_record* record;
    uint64_t sequence;
    if (!yz_frontier_trace_is_armed())
        return;
    if (s_parity_only &&
        type != YZ_FT_PARITY_FLIP &&
        type != YZ_FT_PARITY_MOTION &&
        type != YZ_FT_PARITY_MOTION_DMA &&
        type != YZ_FT_PARITY_SURFACE &&
        type != YZ_FT_PARITY_SURFACE_AUX &&
        type != YZ_FT_PARITY_RENDER &&
        type != YZ_FT_PARITY_GUEST_BLIT &&
        type != YZ_FT_PARITY_CB_RECYCLE)
        return;
    if (s_compact && (type == YZ_FT_FIFO_PARK || type == YZ_FT_SPU_HALT))
        return;

    sequence = yz_frontier_next();
    record = &s_records[sequence & (YZ_FRONTIER_CAPACITY - 1u)];
    yz_frontier_publish(&record->sequence, 0);
    record->qpc = yz_frontier_now();
    record->type = type;
    record->actor = actor;
    record->pc = pc;
    record->arg[0] = a0;
    record->arg[1] = a1;
    record->arg[2] = a2;
    record->arg[3] = a3;
    record->arg[4] = a4;
    record->arg[5] = a5;
    yz_frontier_publish(&record->sequence, sequence + 1u);
}

void yz_frontier_trace_arm(uint32_t actor, uint32_t pc,
                           uint32_t source_ea, uint32_t ls,
                           uint32_t image, uint32_t descriptor)
{
    if (!yz_frontier_trace_enabled())
        return;
#ifdef _WIN32
    if (InterlockedCompareExchange(&s_armed, 1, 0) != 0)
        return;
#else
    {
        int expected = 0;
        if (!atomic_compare_exchange_strong_explicit(
                &s_armed, &expected, 1,
                memory_order_acq_rel, memory_order_acquire))
            return;
    }
#endif
    yz_frontier_trace_emit(YZ_FT_ARM, actor, pc, source_ea, ls,
                           image, descriptor, 0, 0);
}

int yz_frontier_trace_dump(uint32_t reason)
{
    yz_frontier_file_header header;
    char binary_path[YZ_FRONTIER_PATH_MAX + 16u];
    char text_path[YZ_FRONTIER_PATH_MAX + 16u];
    FILE* binary;
    FILE* text_file;
    uint64_t end;
    uint64_t begin;
    long dump_n;

    /* Bootstrap protection (mode '2'): no stall-triggered dump I/O before
     * real progress. The exit dump (reason 3) always goes through so an
     * early freeze still gets its tail captured at window close. */
    if (s_rearm_after_dump && reason != 3u &&
        s_progress < YZ_FRONTIER_PROGRESS_GATE)
        return 0;

#ifdef _WIN32
    dump_n = InterlockedIncrement(&s_dumped);
    if (dump_n > (s_rearm_after_dump ? YZ_FRONTIER_MAX_DUMPS : 1))
        return 0;
    InterlockedExchange(&s_armed, 0);
#else
    dump_n = (long)atomic_fetch_add_explicit(&s_dumped, 1,
                                             memory_order_acq_rel) + 1;
    if (dump_n > (s_rearm_after_dump ? YZ_FRONTIER_MAX_DUMPS : 1))
        return 0;
    atomic_store_explicit(&s_armed, 0, memory_order_release);
#endif

    end = yz_frontier_count();
    begin = end > YZ_FRONTIER_CAPACITY ? end - YZ_FRONTIER_CAPACITY : 0u;
    if (s_rearm_after_dump) {
        snprintf(binary_path, sizeof(binary_path), "%s_d%ld.bin",
                 s_dump_prefix, dump_n);
        snprintf(text_path, sizeof(text_path), "%s_d%ld.tsv",
                 s_dump_prefix, dump_n);
    } else {
        snprintf(binary_path, sizeof(binary_path), "%s.bin", s_dump_prefix);
        snprintf(text_path, sizeof(text_path), "%s.tsv", s_dump_prefix);
    }

    memset(&header, 0, sizeof(header));
    memcpy(header.magic, "YZFTRC1", 7);
    header.version = 1;
    header.record_size = (uint32_t)sizeof(yz_frontier_record);
    header.capacity = YZ_FRONTIER_CAPACITY;
    header.reason = reason;
    header.write_count = end;
    header.retained_begin = begin;
    header.qpc_frequency = s_qpc_frequency;

    binary = fopen(binary_path, "wb");
    if (binary) {
        fwrite(&header, sizeof(header), 1u, binary);
        fwrite(s_records, sizeof(s_records), 1u, binary);
        fclose(binary);
    }

    /* The 64k-line formatted tsv is a measured bootstrap perturbation
     * (boots 49-51) — opt-in only; ring_decode.py reads the .bin. */
    text_file = (s_rearm_after_dump && !s_tsv_enabled)
                    ? NULL : fopen(text_path, "wb");
    if (text_file) {
        uint64_t sequence;
        fprintf(text_file,
                "sequence\tqpc\ttype\tname\tactor\tpc"
                "\ta0\ta1\ta2\ta3\ta4\ta5\n");
        for (sequence = begin; sequence < end; ++sequence) {
            const yz_frontier_record* record =
                &s_records[sequence & (YZ_FRONTIER_CAPACITY - 1u)];
            if (record->sequence != sequence + 1u)
                continue;
            fprintf(text_file,
                    "%" PRIu64 "\t%" PRIu64 "\t%u\t%s"
                    "\t%08" PRIX32 "\t%08" PRIX32
                    "\t%08" PRIX32 "\t%08" PRIX32
                    "\t%08" PRIX32 "\t%08" PRIX32
                    "\t%08" PRIX32 "\t%08" PRIX32 "\n",
                    sequence, record->qpc, record->type,
                    yz_frontier_event_name(record->type),
                    record->actor, record->pc,
                    record->arg[0], record->arg[1], record->arg[2],
                    record->arg[3], record->arg[4], record->arg[5]);
        }
        fclose(text_file);
    }

    fprintf(stderr,
            "[frontier-ring] dumped n=%ld reason=%u records=%" PRIu64
            " retained=%" PRIu64 " binary=%s text=%s\n",
            dump_n, reason, end, end - begin, binary_path, text_path);
    fflush(stderr);
    /* Mode '2': keep recording — the terminal stall must get its dump even
     * when an early-boot quiet phase spent this one. */
    if (s_rearm_after_dump && dump_n < YZ_FRONTIER_MAX_DUMPS) {
#ifdef _WIN32
        InterlockedExchange(&s_armed, 1);
#else
        atomic_store_explicit(&s_armed, 1, memory_order_release);
#endif
    }
    return 1;
}

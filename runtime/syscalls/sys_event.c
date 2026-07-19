/*
 * ps3recomp - Event queue and event flag syscalls (implementation)
 */

#include "sys_event.h"
#include "../ps3_log.h"
#include "sys_timer.h"             /* lv2_usec_deadline: sub-ms timed waits */
#include "../memory/vm.h"
#include "../spu/spu_workload.h"   /* spu_elf_image_size, spu_workload_dispatch_async */
#include <string.h>
#include <stdlib.h>               /* getenv */

/* Recorded by sys_spu_thread_initialize (lv2_register.c): the SPURS kernel
 * context EA the title's SPU task runtime is dispatched against. */
uint32_t g_ydkj_spurs_ctx_ea = 0;
/* Optional title-set hook to run the real lifted SPURS kernel on an SPU thread. */
void (*g_spurs_kernel_hook)(uint32_t) = 0;

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_event_queue_info g_sys_event_queues[SYS_EVENT_QUEUE_MAX];
sys_event_port_info  g_sys_event_ports[SYS_EVENT_PORT_MAX];
sys_event_flag_info  g_sys_event_flags[SYS_EVENT_FLAG_MAX];

/* Table lock for allocation */
#ifdef _WIN32
static CRITICAL_SECTION s_evt_table_lock;
static int              s_evt_table_lock_init = 0;
#else
static pthread_mutex_t  s_evt_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void evt_table_lock(void)
{
#ifdef _WIN32
    if (!s_evt_table_lock_init) {
        InitializeCriticalSection(&s_evt_table_lock);
        s_evt_table_lock_init = 1;
    }
    EnterCriticalSection(&s_evt_table_lock);
#else
    pthread_mutex_lock(&s_evt_table_lock);
#endif
}

static void evt_table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_evt_table_lock);
#else
    pthread_mutex_unlock(&s_evt_table_lock);
#endif
}

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

static uint64_t bswap64(uint64_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    return ((v >> 56) & 0xFFULL) |
           ((v >> 40) & 0xFF00ULL) |
           ((v >> 24) & 0xFF0000ULL) |
           ((v >>  8) & 0xFF000000ULL) |
           ((v <<  8) & 0xFF00000000ULL) |
           ((v << 24) & 0xFF0000000000ULL) |
           ((v << 40) & 0xFF000000000000ULL) |
           ((v << 56) & 0xFF00000000000000ULL);
#else
    return v;
#endif
}

/* =========================================================================
 * EVENT QUEUES
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * sys_event_queue_create
 *
 * r3 = pointer to receive queue ID (u32*)
 * r4 = pointer to attribute struct
 * r5 = key (u64)
 * r6 = size (s32)
 * -----------------------------------------------------------------------*/
int64_t sys_event_queue_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr   = LV2_ARG_PTR(ctx, 1);
    uint64_t key         = LV2_ARG_U64(ctx, 2);
    int32_t  size        = LV2_ARG_S32(ctx, 3);

    if (size <= 0 || size > SYS_EVENT_QUEUE_BUF_MAX)
        size = SYS_EVENT_QUEUE_BUF_MAX;

    evt_table_lock();

    int slot = -1;
    for (int i = 0; i < SYS_EVENT_QUEUE_MAX; i++) {
        if (!g_sys_event_queues[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_event_queue_info* q = &g_sys_event_queues[slot];
    memset(q, 0, sizeof(*q));
    q->active   = 1;
    q->key      = key;
    q->capacity = size;
    q->head     = 0;
    q->tail     = 0;
    q->count    = 0;
    q->type     = SYS_PPU_QUEUE;
    fprintf(stderr, "[evt] queue_create -> id=%d key=0x%llX size=%d\n",
            slot + 1, (unsigned long long)key, size);

    if (attr_addr != 0) {
        uint8_t* attr_raw = (uint8_t*)vm_to_host(attr_addr);
        uint32_t proto_be;
        memcpy(&proto_be, attr_raw, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        proto_be = ((proto_be >> 24) & 0xFF) | ((proto_be >> 8) & 0xFF00) |
                   ((proto_be << 8) & 0xFF0000) | ((proto_be << 24) & 0xFF000000u);
#endif
        q->protocol = proto_be;

        uint32_t type_be;
        memcpy(&type_be, attr_raw + 4, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        type_be = ((type_be >> 24) & 0xFF) | ((type_be >> 8) & 0xFF00) |
                  ((type_be << 8) & 0xFF0000) | ((type_be << 24) & 0xFF000000u);
#endif
        q->type = (int32_t)type_be;

        memcpy(q->name, attr_raw + 8, 8);
    }

#ifdef _WIN32
    InitializeCriticalSection(&q->lock);
    InitializeConditionVariable(&q->not_empty);
#else
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
#endif

    uint32_t queue_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, queue_id);
    }

    evt_table_unlock();
    return CELL_OK;
}

int64_t sys_event_queue_destroy(ppu_context* ctx)
{
    uint32_t queue_id = LV2_ARG_U32(ctx, 0);

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    evt_table_lock();

    sys_event_queue_info* q = &g_sys_event_queues[queue_id - 1];
    if (!q->active) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

#ifdef _WIN32
    DeleteCriticalSection(&q->lock);
#else
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
#endif

    q->active = 0;
    evt_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_event_queue_receive
 *
 * r3 = queue_id
 * r4 = pointer to sys_event_t in guest memory
 * r5 = timeout_usec (0 = infinite)
 * -----------------------------------------------------------------------*/
int64_t sys_event_queue_receive(ppu_context* ctx)
{
    uint32_t queue_id    = LV2_ARG_U32(ctx, 0);
    uint32_t event_addr  = LV2_ARG_PTR(ctx, 1);
    uint64_t timeout_us  = LV2_ARG_U64(ctx, 2);
    if (ps3_log_verbose())
        fprintf(stderr, "[WAIT] event_queue_receive(q=%u timeout=%llu) tid=%llu cia=0x%08X lr=0x%08X\n",
                queue_id, (unsigned long long)timeout_us,
                (unsigned long long)ctx->thread_id, (uint32_t)ctx->cia, (uint32_t)ctx->lr);

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_queue_info* q = &g_sys_event_queues[queue_id - 1];
    if (!q->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* SPURS bring-up: when a PPU thread is about to block forever for SPU
     * completion (libsre created the taskset correctly but never started its SPU
     * kernel), drive the title's REAL lifted SPU task runtime (image 22 @guest
     * 0x004F5F80) against the recorded SPURS context. It claims the ready task
     * from libsre's taskset and runs the task body -- real recompiled SPU code,
     * not a synthesized completion. Gated by YDKJ_SPUTASK; fire once. */
    if (getenv("YDKJ_SPUTASK") && timeout_us == 0 && g_ydkj_spurs_ctx_ea) {
        static int s_fired = 0;
        if (!s_fired) {
            s_fired = 1;
            extern uint8_t* vm_base;
            extern int spu_workload_dispatch_async(const uint8_t*, uint32_t, uint32_t);
            const uint8_t* elf = vm_base + 0x004F5F80u;
            size_t sz = spu_elf_image_size(elf, 2u * 1024 * 1024);
            fprintf(stderr, "[ydkj] block on q=%u -> dispatch real SPU task runtime "
                    "(img@0x4F5F80 sz=%zu ctx=0x%08X)\n", queue_id, sz, g_ydkj_spurs_ctx_ea);
            if (sz) spu_workload_dispatch_async(elf, (uint32_t)sz, g_ydkj_spurs_ctx_ea);
        }
    }

    /* YDKJ_SPURS_READY (diagnostic): break the SPURS bring-up deadlock from the
     * PPU side. cellSpursInitialize blocks forever (timeout=0) on the SPURS event
     * queues (q=1/q=4) awaiting the kernel/policy "ready" event, while the policy
     * (now running real code) waits for a workload the PPU only adds AFTER init
     * returns. Synthesize a ready event so init returns -> see whether the PPU
     * then reaches CreateTaskset and the running policy dispatches the cri task.
     * Returns a zero-ish SPU-thread-group event; refine the format if init rejects it. */
    if (getenv("YDKJ_SPURS_READY") && (queue_id == 1 || queue_id == 4)
            && timeout_us == 0 && q->count == 0) {
        static int s_fire[8] = {0};
        if (s_fire[queue_id] < 64) {
            s_fire[queue_id]++;
            if (event_addr != 0) {
                uint64_t* out = (uint64_t*)vm_to_host(event_addr);
                /* Match the observed real port_send(port=4) shape: data1=0, data2=1,
                 * data3=0. source is the port name; use a SPURS-ish tag. Env
                 * YDKJ_RDY_D2 overrides data2 for quick format experiments. */
                const char* d2s = getenv("YDKJ_RDY_D2");
                uint64_t d2 = d2s ? (uint64_t)strtoull(d2s, 0, 0) : 1ULL;
                out[0] = bswap64(0x0000000000000000ULL); /* source/name */
                out[1] = 0; out[2] = bswap64(d2); out[3] = 0;
            }
            fprintf(stderr, "[ydkj] SPURS_READY: synthesized ready event on q=%u (#%d)\n",
                    queue_id, s_fire[queue_id]);
            return CELL_OK;
        }
    }

    /* YDKJ_FAKECOMPLETE (diagnostic): the SPURS task-completion events the SPU
     * would post to q=2/q=3 never arrive (SPU video task not running), so the
     * main thread times out after 30s and the title tears down. Synthesize a
     * completion so the wait returns -> see whether the game then advances to
     * asset load + menu draw (content). Returns immediately with a zero event. */
    if (getenv("YDKJ_FAKECOMPLETE") && (queue_id == 2 || queue_id == 3) && q->count == 0) {
        if (event_addr != 0) {
            uint64_t* out = (uint64_t*)vm_to_host(event_addr);
            out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
        }
        return CELL_OK;
    }
    /* YDKJ_HLE_DRAW (diagnostic): also unblock the AsyncLoad q=1 wait (timeout=0,
     * blocks forever) so the loader thread proceeds. Tests whether the game's
     * render/draw code is reachable once the completion waits are satisfied. */
    if (getenv("YDKJ_HLE_DRAW") && queue_id == 1 && timeout_us == 0 && q->count == 0) {
        static int s_n1 = 0;
        if (s_n1 < 256) { s_n1++;
            if (event_addr != 0) {
                uint64_t* out = (uint64_t*)vm_to_host(event_addr);
                out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
            }
            return CELL_OK;
        }
    }

    /* Diagnostic: dump the guest call chain of a thread about to block on q=1
     * (the loader/worker) -- identifies WHICH function's receive-loop it's in,
     * so we can see why it only receives once. Fires a few times. */
    if (queue_id == 1 && timeout_us == 0) {
        extern void ppu_dump_guest_stack(ppu_context*, const char*);
        static int _gs = 0; if (_gs++ < 5) ppu_dump_guest_stack(ctx, "q1-worker");
    }

#ifdef _WIN32
    EnterCriticalSection(&q->lock);

    if (timeout_us == 0) {
        while (q->count == 0 && q->active) {
            SleepConditionVariableCS(&q->not_empty, &q->lock, INFINITE);
        }
    } else if (timeout_us < 1000) {
        /* Sub-millisecond timeout = the title's non-blocking event poll (it polls
         * queues 2 & 3 every frame with a 30us timeout). Windows' ~15.6ms timer
         * granularity inflates even a 1ms SleepConditionVariableCS to ~15ms, so a
         * naive floor-to-1ms throttled the game's per-frame poll loop ~500x
         * (each frame ate ~30ms in two polls). Honor the intent: check once and
         * return ETIMEDOUT immediately if empty -- same result the game already
         * handles, just without the bogus 15ms stall. */
        if (q->count == 0 || !q->active) {
            LeaveCriticalSection(&q->lock);
            return (int64_t)(int32_t)CELL_ETIMEDOUT;
        }
    } else {
        DWORD ms = (DWORD)(timeout_us / 1000);
        while (q->count == 0 && q->active) {
            if (!SleepConditionVariableCS(&q->not_empty, &q->lock, ms)) {
                if (GetLastError() == ERROR_TIMEOUT) {
                    LeaveCriticalSection(&q->lock);
                    return (int64_t)(int32_t)CELL_ETIMEDOUT;
                }
            }
        }
    }

    if (!q->active || q->count == 0) {
        LeaveCriticalSection(&q->lock);
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    sys_event_t evt = q->buffer[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);

    if (timeout_us == 0) {
        while (q->count == 0 && q->active) {
            pthread_cond_wait(&q->not_empty, &q->lock);
        }
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeout_us / 1000000);
        ts.tv_nsec += (long)((timeout_us % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (q->count == 0 && q->active) {
            int rc = pthread_cond_timedwait(&q->not_empty, &q->lock, &ts);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&q->lock);
                return (int64_t)(int32_t)CELL_ETIMEDOUT;
            }
        }
    }

    if (!q->active || q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    sys_event_t evt = q->buffer[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    pthread_mutex_unlock(&q->lock);
#endif

    /* lv2 sys_event_queue_receive returns the event in REGISTERS r4..r7
     * (r3=rc, r4=source, r5=data1, r6=data2, r7=data3) -- this is the ABI the
     * caller reads (e.g. the SPURS service func_00C5DAB0 checks r4==source key,
     * extracts the selector from r6). We were only writing the (legacy) memory
     * buffer, so callers that read the registers saw stale values -> the SPURS
     * dispatch failed its source check and re-received forever, never dispatching
     * handler B. Set the registers. */
    ctx->gpr[4] = evt.source;
    ctx->gpr[5] = evt.data1;
    ctx->gpr[6] = evt.data2;
    ctx->gpr[7] = evt.data3;

    /* Also write the (legacy) guest memory buffer in big-endian, for callers that
     * pass a real sys_event_t* and read from it. */
    if (event_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(event_addr);
        out[0] = bswap64(evt.source);
        out[1] = bswap64(evt.data1);
        out[2] = bswap64(evt.data2);
        out[3] = bswap64(evt.data3);
    }
    /* Diagnostic: prove the blocked receiver was WOKEN and is returning an event
     * (vs. staying blocked forever). Distinguishes a wake bug from game logic. */
    if (queue_id == 1) {
        static int _r = 0;
        if (_r++ < 12)
            fprintf(stderr, "[evt] q=1 receive RETURNED to tid=%llu: src=0x%llX d1=0x%llX d2=0x%llX d3=0x%llX (qcount now %u)\n",
                    (unsigned long long)ctx->thread_id, (unsigned long long)evt.source,
                    (unsigned long long)evt.data1, (unsigned long long)evt.data2,
                    (unsigned long long)evt.data3, q->count);
    }

    return CELL_OK;
}

int64_t sys_event_queue_tryreceive(ppu_context* ctx)
{
    uint32_t queue_id   = LV2_ARG_U32(ctx, 0);
    uint32_t event_addr = LV2_ARG_PTR(ctx, 1);
    int32_t  max_count  = LV2_ARG_S32(ctx, 2);
    uint32_t count_addr = LV2_ARG_PTR(ctx, 3);

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_queue_info* q = &g_sys_event_queues[queue_id - 1];
    if (!q->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (max_count <= 0) max_count = 1;

    int32_t received = 0;

#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif

    while (received < max_count && q->count > 0) {
        sys_event_t evt = q->buffer[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;

        if (event_addr != 0) {
            uint64_t* out = (uint64_t*)vm_to_host(event_addr + (uint32_t)(received * 32));
            out[0] = bswap64(evt.source);
            out[1] = bswap64(evt.data1);
            out[2] = bswap64(evt.data2);
            out[3] = bswap64(evt.data3);
        }
        received++;
    }

#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif

    if (count_addr != 0) {
        write_be32(count_addr, (uint32_t)received);
    }

    return CELL_OK;
}

int64_t sys_event_queue_drain(ppu_context* ctx)
{
    uint32_t queue_id = LV2_ARG_U32(ctx, 0);

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_queue_info* q = &g_sys_event_queues[queue_id - 1];
    if (!q->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    EnterCriticalSection(&q->lock);
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_unlock(&q->lock);
#endif

    return CELL_OK;
}

/* =========================================================================
 * EVENT PORTS
 * ========================================================================= */

/* Helper to enqueue an event into a queue */
static int event_queue_push(sys_event_queue_info* q, const sys_event_t* evt)
{
#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif

    if (q->count >= q->capacity) {
#ifdef _WIN32
        LeaveCriticalSection(&q->lock);
#else
        pthread_mutex_unlock(&q->lock);
#endif
        return -1; /* full */
    }

    q->buffer[q->tail] = *evt;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

#ifdef _WIN32
    WakeConditionVariable(&q->not_empty);
    LeaveCriticalSection(&q->lock);
#else
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
#endif

    return 0;
}

/* EXPERIMENT: inject an event into a queue by id, to probe whether unblocking
 * the SPURS completion queue (q=1) lets the PPU reach its render loop. */
int sys_event_queue_inject(uint32_t qid, uint64_t source,
                           uint64_t d1, uint64_t d2, uint64_t d3)
{
    if (qid == 0 || qid > SYS_EVENT_QUEUE_MAX) return -1;
    sys_event_queue_info* q = &g_sys_event_queues[qid - 1];
    if (!q->active) return -1;
    sys_event_t evt; evt.source = source; evt.data1 = d1; evt.data2 = d2; evt.data3 = d3;
    return event_queue_push(q, &evt);
}

int64_t sys_event_port_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    int32_t  port_type   = LV2_ARG_S32(ctx, 1);
    uint64_t name        = LV2_ARG_U64(ctx, 2);

    evt_table_lock();

    int slot = -1;
    for (int i = 0; i < SYS_EVENT_PORT_MAX; i++) {
        if (!g_sys_event_ports[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_event_port_info* p = &g_sys_event_ports[slot];
    memset(p, 0, sizeof(*p));
    p->active = 1;
    p->type   = port_type;
    p->name   = name;
    p->connected_queue = 0;

    uint32_t port_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, port_id);
    }

    evt_table_unlock();
    return CELL_OK;
}

int64_t sys_event_port_destroy(ppu_context* ctx)
{
    uint32_t port_id = LV2_ARG_U32(ctx, 0);

    if (port_id == 0 || port_id > SYS_EVENT_PORT_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    evt_table_lock();

    sys_event_port_info* p = &g_sys_event_ports[port_id - 1];
    if (!p->active) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    p->active = 0;
    evt_table_unlock();
    return CELL_OK;
}

int64_t sys_event_port_connect_local(ppu_context* ctx)
{
    uint32_t port_id  = LV2_ARG_U32(ctx, 0);
    uint32_t queue_id = LV2_ARG_U32(ctx, 1);

    if (port_id == 0 || port_id > SYS_EVENT_PORT_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;
    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    evt_table_lock();

    sys_event_port_info* p = &g_sys_event_ports[port_id - 1];
    if (!p->active) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (!g_sys_event_queues[queue_id - 1].active) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (p->connected_queue != 0) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_EISCONN;
    }

    p->connected_queue = (int32_t)queue_id;
    evt_table_unlock();
    return CELL_OK;
}

int64_t sys_event_port_disconnect(ppu_context* ctx)
{
    uint32_t port_id = LV2_ARG_U32(ctx, 0);

    if (port_id == 0 || port_id > SYS_EVENT_PORT_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    evt_table_lock();

    sys_event_port_info* p = &g_sys_event_ports[port_id - 1];
    if (!p->active) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (p->connected_queue == 0) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ENOTCONN;
    }

    p->connected_queue = 0;
    evt_table_unlock();
    return CELL_OK;
}

/* Public helper for non-syscall callers: push an event into a queue by
 * ID. Returns 0 on success, -1 if the queue is unknown/inactive or full. */
int sys_event_queue_push_by_id(uint32_t queue_id,
                               uint64_t source, uint64_t data1,
                               uint64_t data2,  uint64_t data3)
{
    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX) return -1;
    sys_event_queue_info* q = &g_sys_event_queues[queue_id - 1];
    if (!q->active) return -1;
    sys_event_t evt;
    evt.source = source;
    evt.data1  = data1;
    evt.data2  = data2;
    evt.data3  = data3;
    return event_queue_push(q, &evt);
}

/* Public helper: resolve an event queue by its ipc_key (as registered at
 * sys_event_queue_create). Returns the queue_id (1-based) or 0 if none. Used by
 * cellAudio to route the audio-period notify event to the game's queue. */
uint32_t sys_event_find_queue_by_key(uint64_t key)
{
    if (key == 0) return 0;
    for (int i = 0; i < SYS_EVENT_QUEUE_MAX; i++) {
        if (g_sys_event_queues[i].active && g_sys_event_queues[i].key == key)
            return (uint32_t)(i + 1);
    }
    return 0;
}

int64_t sys_event_port_send(ppu_context* ctx)
{
    uint32_t port_id = LV2_ARG_U32(ctx, 0);
    uint64_t data1   = LV2_ARG_U64(ctx, 1);
    uint64_t data2   = LV2_ARG_U64(ctx, 2);
    uint64_t data3   = LV2_ARG_U64(ctx, 3);
    fprintf(stderr, "[evt] port_send(port=%u data=0x%llX/0x%llX/0x%llX)\n",
            port_id, (unsigned long long)data1, (unsigned long long)data2, (unsigned long long)data3);

    if (port_id == 0 || port_id > SYS_EVENT_PORT_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_port_info* p = &g_sys_event_ports[port_id - 1];
    if (!p->active) {
        fprintf(stderr, "[evt] port_send(port=%u): port NOT ACTIVE\n", port_id);
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (p->connected_queue == 0) {
        fprintf(stderr, "[evt] port_send(port=%u): NOT CONNECTED to any queue\n", port_id);
        return (int64_t)(int32_t)CELL_ENOTCONN;
    }
    fprintf(stderr, "[evt] port_send(port=%u) -> queue id=%d (source=0x%llX)\n",
            port_id, p->connected_queue, (unsigned long long)p->name);

    int32_t qidx = p->connected_queue;
    if (qidx <= 0 || qidx > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_queue_info* q = &g_sys_event_queues[qidx - 1];
    if (!q->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_t evt;
    evt.source = p->name;
    evt.data1  = data1;
    evt.data2  = data2;
    evt.data3  = data3;

    if (event_queue_push(q, &evt) < 0) {
        return (int64_t)(int32_t)CELL_EBUSY;
    }

    return CELL_OK;
}

/* =========================================================================
 * EVENT FLAGS
 * ========================================================================= */

int64_t sys_event_flag_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr   = LV2_ARG_PTR(ctx, 1);
    uint64_t init_pattern = LV2_ARG_U64(ctx, 2);

    evt_table_lock();

    int slot = -1;
    for (int i = 0; i < SYS_EVENT_FLAG_MAX; i++) {
        if (!g_sys_event_flags[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_event_flag_info* f = &g_sys_event_flags[slot];
    memset(f, 0, sizeof(*f));
    f->active  = 1;
    f->pattern = init_pattern;

    if (attr_addr != 0) {
        uint8_t* attr_raw = (uint8_t*)vm_to_host(attr_addr);
        uint32_t proto_be, type_be;
        memcpy(&proto_be, attr_raw, 4);
        memcpy(&type_be, attr_raw + 4, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        proto_be = ((proto_be >> 24) & 0xFF) | ((proto_be >> 8) & 0xFF00) |
                   ((proto_be << 8) & 0xFF0000) | ((proto_be << 24) & 0xFF000000u);
        type_be  = ((type_be >> 24) & 0xFF)  | ((type_be >> 8) & 0xFF00) |
                   ((type_be << 8) & 0xFF0000) | ((type_be << 24) & 0xFF000000u);
#endif
        f->protocol = proto_be;
        f->type     = type_be;
        memcpy(f->name, attr_raw + 8, 8);
    }

#ifdef _WIN32
    InitializeCriticalSection(&f->lock);
    InitializeConditionVariable(&f->cv);
#else
    pthread_mutex_init(&f->lock, NULL);
    pthread_cond_init(&f->cv, NULL);
#endif

    uint32_t flag_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, flag_id);
    }
    { char nm[9]; memcpy(nm, f->name, 8); nm[8]=0;
      for(int i=0;i<8;i++) if(nm[i] && (nm[i]<32||nm[i]>126)) nm[i]='.';
      fprintf(stderr, "[evt] flag_create id=%u name=\"%s\" init=0x%llX type=%u\n",
              flag_id, nm, (unsigned long long)init_pattern, f->type); }

    evt_table_unlock();
    return CELL_OK;
}

int64_t sys_event_flag_destroy(ppu_context* ctx)
{
    uint32_t flag_id = LV2_ARG_U32(ctx, 0);

    if (flag_id == 0 || flag_id > SYS_EVENT_FLAG_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    evt_table_lock();

    sys_event_flag_info* f = &g_sys_event_flags[flag_id - 1];
    if (!f->active) {
        evt_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

#ifdef _WIN32
    DeleteCriticalSection(&f->lock);
#else
    pthread_cond_destroy(&f->cv);
    pthread_mutex_destroy(&f->lock);
#endif

    f->active = 0;
    evt_table_unlock();
    return CELL_OK;
}

/* Check if wait condition is met */
static int flag_check(uint64_t pattern, uint64_t bitpat, uint32_t mode)
{
    if (mode & SYS_EVENT_FLAG_WAIT_AND) {
        return (pattern & bitpat) == bitpat;
    } else {
        /* OR mode */
        return (pattern & bitpat) != 0;
    }
}

/* ---------------------------------------------------------------------------
 * sys_event_flag_wait
 *
 * r3 = flag_id
 * r4 = bitpattern (u64)
 * r5 = mode (AND/OR + clear flags)
 * r6 = pointer to receive result pattern (u64*)
 * r7 = timeout_usec (0 = infinite)
 * -----------------------------------------------------------------------*/
int64_t sys_event_flag_wait(ppu_context* ctx)
{
    uint32_t flag_id    = LV2_ARG_U32(ctx, 0);
    uint64_t bitpat     = LV2_ARG_U64(ctx, 1);
    uint32_t mode       = LV2_ARG_U32(ctx, 2);
    uint32_t result_addr = LV2_ARG_PTR(ctx, 3);
    { static int n=0; if(n++<30) fprintf(stderr,"[WAIT] event_flag_wait(flag=%u pat=0x%llX mode=%u)\n", flag_id,(unsigned long long)bitpat,mode); }
    uint64_t timeout_us = LV2_ARG_U64(ctx, 4);
    { static int _w=0; if (_w++ < 40) fprintf(stderr, "[WAIT] event_flag_wait(flag=%u bits=0x%llX timeout=%llu)\n", flag_id, (unsigned long long)bitpat, (unsigned long long)timeout_us); }
    /* SPU-completion shim (targeted): the main thread spins in func_003319D0 until
     * *(r29+0x24) (a completion counter the SPU would increment) reaches the target
     * *(r29). Since we don't yet run the SPU workload, satisfy that counter so the
     * main thread proceeds into its REAL recompiled render code. r29 is live in ctx
     * (the loop calls lv2_syscall(ctx) with r3=flag=1000). Endian-agnostic raw copy. */
    if (bitpat == 0) {
        /* bits==0 is the degenerate completion-poll (func_003319D0): the loop
         * spins until *(r29+0x24) == *(r29) -- a counter the SPU would advance.
         * Satisfy it so the main thread proceeds into real recompiled code. */
        uint32_t obj = (uint32_t)ctx->gpr[29];
        uint8_t* tgt = (uint8_t*)vm_to_host(obj);
        uint8_t* cur = (uint8_t*)vm_to_host(obj + 0x24);
        static int _n=0; if (_n++ < 12)
            fprintf(stderr, "[evt] SPU-completion shim: flag=%u r29=0x%08X target=%02X%02X%02X%02X cur=%02X%02X%02X%02X -> satisfied\n",
                    flag_id, obj, tgt[0],tgt[1],tgt[2],tgt[3], cur[0],cur[1],cur[2],cur[3]);
        cur[0]=tgt[0]; cur[1]=tgt[1]; cur[2]=tgt[2]; cur[3]=tgt[3];
        return CELL_OK;
    }

    if (flag_id == 0 || flag_id > SYS_EVENT_FLAG_MAX) {
        { static int _e=0; if(_e++<8) fprintf(stderr,"[evt] flag_wait flag=%u OUT-OF-RANGE (max=%d) -> ESRCH\n", flag_id, SYS_EVENT_FLAG_MAX); }
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    /* YDKJ_F100_OK (diagnostic, env-gated): the game busy-spins ~145k times on
     * event_flag_wait(flag=100 bits=0x2) with a GARBAGE mode (0x38CAE4) on a
     * NEVER-CREATED flag -> ESRCH each time. Test whether returning CELL_OK
     * (as if the flag were set) breaks the spin and lets the game progress into
     * real render code. Diagnostic only; identifies whether the spin is the gate. */
    { static int s_f = -1; if (s_f < 0) s_f = getenv("YDKJ_F100_OK") ? 1 : 0;
      if (s_f && !g_sys_event_flags[flag_id-1].active) {
        static int _n=0; if(_n++<4) fprintf(stderr,"[evt] YDKJ_F100_OK: flag=%u -> return CELL_OK (break spin)\n", flag_id);
        if (result_addr != 0) { write_be32(result_addr, (uint32_t)(bitpat>>32)); write_be32(result_addr+4, (uint32_t)bitpat); }
        return CELL_OK;
      } }
    sys_event_flag_info* f = &g_sys_event_flags[flag_id - 1];
    if (!f->active) {
        { static int _e=0; if(_e++<8) {
            uint32_t r29=(uint32_t)ctx->gpr[29], r30=(uint32_t)ctx->gpr[30], r31=(uint32_t)ctx->gpr[31], r3g=(uint32_t)ctx->gpr[3];
            fprintf(stderr,"[evt] flag_wait flag=%u NOT ACTIVE -> ESRCH; r3=0x%08X r29=0x%08X r30=0x%08X r31=0x%08X\n", flag_id, r3g, r29, r30, r31);
            /* dump the loop's likely counter object (r29-relative, like the flag=1000 shim's *(r29)/*(r29+0x24)) */
            if (r29 && r29 < 0x10000000u) { uint8_t* p=(uint8_t*)vm_to_host(r29);
              fprintf(stderr,"     [r29+0x00..0x30]:"); for(int i=0;i<0x34;i+=4) fprintf(stderr," %02X%02X%02X%02X",p[i],p[i+1],p[i+2],p[i+3]); fprintf(stderr,"\n"); }
        } }
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    { static int _a=0; if(_a++<8) fprintf(stderr,"[evt] flag_wait flag=%u ACTIVE, pattern=0x%llX awaiting bits=0x%llX -> BLOCK\n", flag_id,(unsigned long long)f->pattern,(unsigned long long)bitpat); }

    if (bitpat == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    /* YDKJ_FORCE_EVF (diagnostic): the game's init blocks polling event_flag
     * (flag=100 bits=0x2) for a subsystem/SPU completion that our HLE never fires,
     * so boot stalls on a black screen. Force-satisfy the wait (set the awaited
     * bits) to see if the game advances into its real render/content code. Blunt;
     * identifies the gate. */
    { static int s_fe = -1; if (s_fe < 0) s_fe = getenv("YDKJ_FORCE_EVF") ? 1 : 0;
      if (s_fe && f->active && !flag_check(f->pattern, bitpat, mode)) {
        static int _n = 0; if (_n++ < 20)
            fprintf(stderr, "[evt] YDKJ_FORCE_EVF: force-satisfy flag=%u bits=0x%llX mode=%u\n",
                    flag_id, (unsigned long long)bitpat, mode);
#ifdef _WIN32
        EnterCriticalSection(&f->lock); f->pattern |= bitpat; LeaveCriticalSection(&f->lock);
#else
        pthread_mutex_lock(&f->lock); f->pattern |= bitpat; pthread_mutex_unlock(&f->lock);
#endif
      } }

#ifdef _WIN32
    EnterCriticalSection(&f->lock);

    if (timeout_us == 0) {
        while (!flag_check(f->pattern, bitpat, mode) && f->active) {
            SleepConditionVariableCS(&f->cv, &f->lock, INFINITE);
        }
    } else if (timeout_us < 1000) {
        /* Sub-ms timed wait: poll the pattern to a QPC deadline instead of a
         * floored-to-1ms condvar wait. Safe to poll: f->pattern only changes
         * under f->lock (sys_event_flag_set/clear), so yielding with the lock
         * dropped never misses a committed pattern change -- it just re-reads
         * the latest value on the next lock acquisition. */
        int64_t deadline = lv2_usec_deadline(timeout_us);
        while (!flag_check(f->pattern, bitpat, mode) && f->active) {
            if (lv2_deadline_passed(deadline)) {
                /* Write current pattern even on timeout */
                if (result_addr != 0) {
                    uint64_t* out = (uint64_t*)vm_to_host(result_addr);
                    *out = bswap64(f->pattern);
                }
                LeaveCriticalSection(&f->lock);
                return (int64_t)(int32_t)CELL_ETIMEDOUT;
            }
            LeaveCriticalSection(&f->lock);
            SwitchToThread();
            EnterCriticalSection(&f->lock);
        }
    } else {
        DWORD ms = (DWORD)(timeout_us / 1000);
        while (!flag_check(f->pattern, bitpat, mode) && f->active) {
            if (!SleepConditionVariableCS(&f->cv, &f->lock, ms)) {
                if (GetLastError() == ERROR_TIMEOUT) {
                    /* Write current pattern even on timeout */
                    if (result_addr != 0) {
                        uint64_t* out = (uint64_t*)vm_to_host(result_addr);
                        *out = bswap64(f->pattern);
                    }
                    LeaveCriticalSection(&f->lock);
                    return (int64_t)(int32_t)CELL_ETIMEDOUT;
                }
            }
        }
    }

    uint64_t result = f->pattern;

    /* Clear matched bits if requested */
    if (mode & SYS_EVENT_FLAG_WAIT_CLEAR) {
        f->pattern &= ~bitpat;
    } else if (mode & SYS_EVENT_FLAG_WAIT_CLEAR_ALL) {
        f->pattern = 0;
    }

    if (result_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(result_addr);
        *out = bswap64(result);
    }

    LeaveCriticalSection(&f->lock);
#else
    pthread_mutex_lock(&f->lock);

    if (timeout_us == 0) {
        while (!flag_check(f->pattern, bitpat, mode) && f->active) {
            pthread_cond_wait(&f->cv, &f->lock);
        }
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeout_us / 1000000);
        ts.tv_nsec += (long)((timeout_us % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (!flag_check(f->pattern, bitpat, mode) && f->active) {
            int rc = pthread_cond_timedwait(&f->cv, &f->lock, &ts);
            if (rc == ETIMEDOUT) {
                if (result_addr != 0) {
                    uint64_t* out = (uint64_t*)vm_to_host(result_addr);
                    *out = bswap64(f->pattern);
                }
                pthread_mutex_unlock(&f->lock);
                return (int64_t)(int32_t)CELL_ETIMEDOUT;
            }
        }
    }

    uint64_t result = f->pattern;

    if (mode & SYS_EVENT_FLAG_WAIT_CLEAR) {
        f->pattern &= ~bitpat;
    } else if (mode & SYS_EVENT_FLAG_WAIT_CLEAR_ALL) {
        f->pattern = 0;
    }

    if (result_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(result_addr);
        *out = bswap64(result);
    }

    pthread_mutex_unlock(&f->lock);
#endif

    return CELL_OK;
}

int64_t sys_event_flag_trywait(ppu_context* ctx)
{
    uint32_t flag_id     = LV2_ARG_U32(ctx, 0);
    uint64_t bitpat      = LV2_ARG_U64(ctx, 1);
    uint32_t mode        = LV2_ARG_U32(ctx, 2);
    uint32_t result_addr = LV2_ARG_PTR(ctx, 3);

    if (flag_id == 0 || flag_id > SYS_EVENT_FLAG_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_flag_info* f = &g_sys_event_flags[flag_id - 1];
    if (!f->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    EnterCriticalSection(&f->lock);
#else
    pthread_mutex_lock(&f->lock);
#endif

    if (!flag_check(f->pattern, bitpat, mode)) {
        if (result_addr != 0) {
            uint64_t* out = (uint64_t*)vm_to_host(result_addr);
            *out = bswap64(f->pattern);
        }
#ifdef _WIN32
        LeaveCriticalSection(&f->lock);
#else
        pthread_mutex_unlock(&f->lock);
#endif
        return (int64_t)(int32_t)CELL_EBUSY;
    }

    uint64_t result = f->pattern;

    if (mode & SYS_EVENT_FLAG_WAIT_CLEAR) {
        f->pattern &= ~bitpat;
    } else if (mode & SYS_EVENT_FLAG_WAIT_CLEAR_ALL) {
        f->pattern = 0;
    }

    if (result_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(result_addr);
        *out = bswap64(result);
    }

#ifdef _WIN32
    LeaveCriticalSection(&f->lock);
#else
    pthread_mutex_unlock(&f->lock);
#endif

    return CELL_OK;
}

int64_t sys_event_flag_set(ppu_context* ctx)
{
    uint32_t flag_id = LV2_ARG_U32(ctx, 0);
    uint64_t bitpat  = LV2_ARG_U64(ctx, 1);

    { static int _n=0; if(_n++<200) fprintf(stderr,"[evt] flag_set(flag=%u bits=0x%llX)\n",
        flag_id,(unsigned long long)bitpat); }

    if (flag_id == 0 || flag_id > SYS_EVENT_FLAG_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_flag_info* f = &g_sys_event_flags[flag_id - 1];
    if (!f->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    EnterCriticalSection(&f->lock);
    f->pattern |= bitpat;
    WakeAllConditionVariable(&f->cv);
    LeaveCriticalSection(&f->lock);
#else
    pthread_mutex_lock(&f->lock);
    f->pattern |= bitpat;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->lock);
#endif

    return CELL_OK;
}

/* YDKJ diag (YDKJ_CRI_WAKE): the cri_mpv SPU task completes but the SPU->PPU
 * completion (cellSpursEventFlagSet) isn't propagated, so the game blocks in
 * event_flag_wait forever (black screen). As a probe, set all bits on every
 * active event flag with a waiter -> wake any completion-waiter, to see if the
 * game then advances to draw content. Blunt; identifies the gate, not a real fix. */
void ydkj_wake_all_event_flags(void)
{
    for (int i = 0; i < SYS_EVENT_FLAG_MAX; i++) {
        sys_event_flag_info* f = &g_sys_event_flags[i];
        if (!f->active) continue;
#ifdef _WIN32
        EnterCriticalSection(&f->lock);
        f->pattern |= 0xFFFFFFFFFFFFFFFFull;
        WakeAllConditionVariable(&f->cv);
        LeaveCriticalSection(&f->lock);
#else
        pthread_mutex_lock(&f->lock);
        f->pattern |= 0xFFFFFFFFFFFFFFFFull;
        pthread_cond_broadcast(&f->cv);
        pthread_mutex_unlock(&f->lock);
#endif
    }
}

int64_t sys_event_flag_clear(ppu_context* ctx)
{
    uint32_t flag_id = LV2_ARG_U32(ctx, 0);
    uint64_t bitpat  = LV2_ARG_U64(ctx, 1);

    if (flag_id == 0 || flag_id > SYS_EVENT_FLAG_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_flag_info* f = &g_sys_event_flags[flag_id - 1];
    if (!f->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    EnterCriticalSection(&f->lock);
    f->pattern &= bitpat;  /* PS3 clear: AND with bitpat (clear bits that are 0 in bitpat) */
    LeaveCriticalSection(&f->lock);
#else
    pthread_mutex_lock(&f->lock);
    f->pattern &= bitpat;
    pthread_mutex_unlock(&f->lock);
#endif

    return CELL_OK;
}

int64_t sys_event_flag_get(ppu_context* ctx)
{
    uint32_t flag_id  = LV2_ARG_U32(ctx, 0);
    uint32_t out_addr = LV2_ARG_PTR(ctx, 1);

    if (flag_id == 0 || flag_id > SYS_EVENT_FLAG_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_event_flag_info* f = &g_sys_event_flags[flag_id - 1];
    if (!f->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    uint64_t pattern;
#ifdef _WIN32
    EnterCriticalSection(&f->lock);
    pattern = f->pattern;
    LeaveCriticalSection(&f->lock);
#else
    pthread_mutex_lock(&f->lock);
    pattern = f->pattern;
    pthread_mutex_unlock(&f->lock);
#endif

    if (out_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(out_addr);
        *out = bswap64(pattern);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_event_init(lv2_syscall_table* tbl)
{
    memset(g_sys_event_queues, 0, sizeof(g_sys_event_queues));
    memset(g_sys_event_ports,  0, sizeof(g_sys_event_ports));
    memset(g_sys_event_flags,  0, sizeof(g_sys_event_flags));

#ifdef _WIN32
    if (!s_evt_table_lock_init) {
        InitializeCriticalSection(&s_evt_table_lock);
        s_evt_table_lock_init = 1;
    }
#endif

    /* Event queues */
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_CREATE,      sys_event_queue_create);
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_DESTROY,     sys_event_queue_destroy);
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_RECEIVE,     sys_event_queue_receive);
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_TRYRECEIVE,  sys_event_queue_tryreceive);
    lv2_syscall_register(tbl, SYS_EVENT_QUEUE_DRAIN,       sys_event_queue_drain);

    /* Event ports */
    lv2_syscall_register(tbl, SYS_EVENT_PORT_CREATE,        sys_event_port_create);
    lv2_syscall_register(tbl, SYS_EVENT_PORT_DESTROY,       sys_event_port_destroy);
    lv2_syscall_register(tbl, SYS_EVENT_PORT_CONNECT_LOCAL, sys_event_port_connect_local);
    lv2_syscall_register(tbl, SYS_EVENT_PORT_DISCONNECT,    sys_event_port_disconnect);
    lv2_syscall_register(tbl, SYS_EVENT_PORT_SEND,          sys_event_port_send);

    /* Event flags */
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_CREATE,   sys_event_flag_create);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_DESTROY,  sys_event_flag_destroy);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_WAIT,     sys_event_flag_wait);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_TRYWAIT,  sys_event_flag_trywait);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_SET,      sys_event_flag_set);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_CLEAR,    sys_event_flag_clear);
    lv2_syscall_register(tbl, SYS_EVENT_FLAG_GET,      sys_event_flag_get);
}

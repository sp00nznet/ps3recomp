/* Synthetic taskset requeue boundary. The driver inserts the unchanged production
 * ELF parser and scheduler functions; only host dispatch is intercepted so no
 * SPU thread or original title image is required. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int32_t LONG;
static LONG InterlockedExchange(volatile LONG* value, LONG replacement)
{ LONG old = *value; *value = replacement; return old; }
static void Sleep(unsigned ms) { (void)ms; }
uint8_t* vm_base;
uint32_t g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
static unsigned dispatches;
static const uint8_t* dispatched_image;
static uint32_t dispatched_size, dispatched_context;
/* The scheduler updates the taskset's bitset line under the lock-line lock;
 * single-threaded here, so the lock and the coherency note are no-ops. */
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
static int spu_workload_dispatch_async(const uint8_t* image, uint32_t size, uint32_t context)
{
    dispatches++; dispatched_image = image; dispatched_size = size;
    dispatched_context = context; return 1;
}
/* Taskset2 exit-code slot: these tasksets are not registered as Taskset2,
 * where production returns without touching memory. */
static void spu_taskset2_set_exit(uint32_t taskset_ea, uint32_t task, int exited, uint32_t code)
{ (void)taskset_ea; (void)task; (void)exited; (void)code; }
/* The task-start log line: production prints the first few, then only
 * with PS3_VERBOSE (ps3_log.h, log_first.h). */
static int ps3_log_verbose(void) { return 0; }
#define PS3_LOG_FIRST(ctr, limit) ((ctr) < (limit) && ++(ctr))
/* PRODUCTION_FUNCTIONS */
static unsigned passed, failed;
#define CHECK(test, message) do { if (test) passed++; else { failed++; fprintf(stderr, "FAIL: %s\n", message); } } while (0)
enum { TASKSET = 0x10000, ELF = 0x30000, CONTEXT = 0x50000 };
static void be16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void be32(uint8_t* p, uint32_t v)
{ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void setup(uint32_t flags, int pending)
{
    memset(vm_base, 0, 0x400000); memset(s_ts_run, 0, sizeof(s_ts_run));
    s_ts_lock = 0; dispatches = 0; dispatched_image = NULL;
    dispatched_size = dispatched_context = 0;
    g_ydkj_real_taskset_ea = g_ydkj_real_taskid = 0;
    uint8_t* image = vm_base + ELF;
    memcpy(image, "\177ELF", 4); image[4] = 1; image[5] = 2; image[6] = 1;
    be16(image + 0x10, 2); be16(image + 0x12, 23); be32(image + 0x14, 1);
    be32(image + 0x18, 0x3000); be32(image + 0x1C, 52);
    be16(image + 0x28, 52); be16(image + 0x2A, 32); be16(image + 0x2C, 1);
    be32(image + 52, 1); be32(image + 56, 84); be32(image + 60, 0x3000);
    be32(image + 68, 16); be32(image + 72, 16); be32(image + 76, 5);
    be32(image + 80, 16); memset(image + 84, 0x5A, 16);
    uint8_t* ts = vm_base + TASKSET;
    ts[pending ? 0x20 : 0x10] = 0x80; ts[0x30] = 0x80;
    be32(ts + 0x80 + 0x14, ELF | flags);
    be32(ts + 0x80 + 0x1C, CONTEXT);
    /* Unrelated TaskInfo metadata must survive address normalization. */
    be32(ts + 0x80, 0x11223344); be32(ts + 0x90, 0xAABBCCDD);
}
int main(void)
{
    vm_base = calloc(1, 0x400000); if (!vm_base) return 2;
    for (uint32_t flags = 0; flags < 8; flags++) for (int pending = 0; pending < 2; pending++) {
        setup(flags, pending); uint8_t before[48];
        memcpy(before, vm_base + TASKSET + 0x80, sizeof(before));
        spu_taskset_task_exited(TASKSET, 127);
        CHECK(dispatches == 1, "ready/pending task dispatches with ELF metadata tags");
        CHECK(dispatched_image == vm_base + ELF, "dispatch uses ELF base, not tagged address");
        CHECK(dispatched_size == 100, "real parser receives synthetic ELF header");
        CHECK(dispatched_context == CONTEXT, "context argument is preserved");
        CHECK(g_ydkj_real_taskset_ea == TASKSET && g_ydkj_real_taskid == 0,
              "requeued task keeps its taskset and slot identity");
        CHECK(memcmp(before, vm_base + TASKSET + 0x80, sizeof(before)) == 0,
              "TaskInfo flags and all other metadata remain unchanged");
        CHECK((s_ts_run[0].running[0] & 0x80) != 0, "submitted task is marked in host run table");
        CHECK(vm_base[TASKSET + 0x20] == 0, "pending-ready bit consumed on submission");
    }
    setup(1, 0); vm_base[TASKSET + 0x30] = 0;
    spu_taskset_task_exited(TASKSET, 127);
    CHECK(dispatches == 0, "disabled task is not dispatched");
    setup(1, 0); ts_running(TASKSET)[0] = 0x80;
    spu_taskset_task_exited(TASKSET, 127);
    CHECK(dispatches == 0, "already running task is not duplicated");
    setup(1, 0); vm_base[TASKSET + 0x10] = 0;
    spu_taskset_task_exited(TASKSET, 127);
    CHECK(dispatches == 0, "enabled task without work is not dispatched");
    setup(1, 0); vm_base[ELF] = 0;
    spu_taskset_task_exited(TASKSET, 127);
    CHECK(dispatches == 0, "invalid untagged ELF is not dispatched");
    CHECK(s_ts_run[0].running[0] == 0, "invalid ELF leaves host run table clear");
    CHECK(vm_base[TASKSET + 0x10] == 0x80, "invalid ELF preserves ready state");
    setup(7, 1); be32(vm_base + TASKSET + 0x94, 7);
    spu_taskset_task_exited(TASKSET, 127);
    CHECK(dispatches == 0, "metadata-only null ELF is not dispatched");
    CHECK(vm_base[TASKSET + 0x20] == 0x80, "null ELF preserves pending state");
    setup(1, 0); vm_base[TASKSET + 0x20] = 0x80; ts_running(TASKSET)[0] = 0x80;
    spu_taskset_task_exited(TASKSET, 0);
    CHECK(dispatches == 0, "exiting task is not restarted");
    CHECK(vm_base[TASKSET + 0x10] == 0 && vm_base[TASKSET + 0x20] == 0 &&
          vm_base[TASKSET + 0x30] == 0 && s_ts_run[0].running[0] == 0,
          "real exiting task ready/pending/enabled/run state is cleared");
    free(vm_base);
    printf("taskset ELF address flags: %u passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}

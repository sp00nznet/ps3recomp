#include "libs/spurs/cellSpurs.h"
#include "ps3emu/log_first.h"
#include "runtime/platform/win32_compat.h"
#include "runtime/spu/spu_context.h"
#include "runtime/syscalls/sys_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t *vm_base;
uint32_t ppu_vm_size;
#ifndef _WIN32
void SetLastError(DWORD error) {
  (void)error;
  abort();
} /* unused group wait */
#endif
#define GUEST_PTR(p, T)                                                        \
  ((T)((p) ? (void *)(vm_base + (uint32_t)(uintptr_t)(p)) : (void *)0))
#include "spurs_ports_under_test.inc"
/* Non-SPURS branches are not exercised here. These stubs only satisfy their
 * references; actual SPURS lookup, mailbox operations, queue push and receive
 * are production code. No fake queue delivery or receive. */
typedef struct {
  struct {
    uint32_t spup, queue;
  } evt_bind[64];
  int evt_bind_n;
} spu_thread_t;
typedef struct {
  uint32_t user_event_ports[64];
} spu_group_t;
static spu_thread_t *spu_find_thread(uint32_t id) {
  (void)id;
  abort();
  return NULL;
}
static spu_group_t *spu_find_group(uint32_t id) {
  (void)id;
  abort();
  return NULL;
}
int64_t sys_event_flag_set(ppu_context *c) {
  (void)c;
  abort();
  return 0;
}
#include "spurs_delivery_under_test.inc"
static int failures;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      printf("FAIL line %d: %s\n", __LINE__, #x);                              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)
static uint32_t be32(uint32_t a) {
  return (uint32_t)vm_base[a] << 24 | (uint32_t)vm_base[a + 1] << 16 |
         (uint32_t)vm_base[a + 2] << 8 | vm_base[a + 3];
}
static uint64_t be64(uint32_t a) {
  return ((uint64_t)be32(a) << 32) | be32(a + 4);
}
static void put32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
static uint32_t newq(void) {
  uint32_t q = sys_event_queue_create_direct(0, 16);
  CHECK(q != 0);
  return q;
}
static uint32_t attach(uint32_t owner, uint32_t q, uint8_t port, int dyn) {
  vm_base[0x100] = port;
  uint32_t rc = cellSpursAttachLv2EventQueue((CellSpurs *)(uintptr_t)owner, q,
                                             (u8 *)(uintptr_t)0x100, dyn);
  CHECK(rc == CELL_OK);
  return vm_base[0x100];
}
static void destroy(uint32_t q) {
  ppu_context c = {0};
  c.gpr[3] = q;
  c.gpr[4] = SYS_EVENT_QUEUE_DESTROY_FORCE;
  CHECK(sys_event_queue_destroy(&c) == 0);
}
static void receive(uint32_t q, uint32_t expected, uint32_t payload) {
  ppu_context c = {0};
  c.gpr[3] = q;
  c.gpr[4] = 0x400;
  c.gpr[5] = 16;
  c.gpr[6] = 0x200;
  CHECK(sys_event_queue_tryreceive(&c) == CELL_OK);
  uint32_t count = be32(0x200);
  printf("receive queue=%u count=%u expected=%u data=%llx\n", q, count,
         expected, (unsigned long long)(count ? be64(0x418) : 0));
  CHECK(count == expected);
  if (expected && count)
    for (uint32_t i = 0; i < count; i++) {
      CHECK(be64(0x400 + 32 * i) == 0xFFFFFFFF53505501ull);
      CHECK(be64(0x418 + 32 * i) == payload);
    }
}
static uint32_t expected_ack;
static void send(uint32_t owner, uint32_t port, uint32_t payload,
                 int throwing) {
  spu_context s;
  memset(&s, 0, sizeof s);
  s.ls = s.ls_store;
  s.spu_id = owner;
  s.image_id = 7;
  s.spurs_ea = owner;
  /* LS contents do not determine routing; the captured context owner does. */
  put32(s.ls + 0x1c4, owner);
  put32(s.ls + 0x2764, owner);
  spu_channel_write(&s.ch_out_mbox, payload);
  int handled =
      spu_deliver_user_event(&s, ((port + (throwing ? 64 : 0)) << 24) | 0x123);
  printf("send owner=%x port=%u handled=%d ack_count=%u\n", owner, port,
         handled, s.ch_in_mbox.count);
  CHECK(handled == 1);
  if (!throwing) {
    CHECK(s.ch_in_mbox.count == 1);
    if (s.ch_in_mbox.count)
      CHECK(spu_channel_read(&s.ch_in_mbox) == expected_ack);
  } else
    CHECK(s.ch_in_mbox.count == 0);
}
/* Two simultaneously active senders must not share a routing identity. */
typedef struct {
  uint32_t owner, port, payload;
  int failed;
} Sender;
static volatile LONG senders_ready;
static void sender_run(Sender *w) {
  spu_context c;
  memset(&c, 0, sizeof c);
  c.spurs_ea = w->owner;
  InterlockedIncrement(&senders_ready);
  while (InterlockedCompareExchange(&senders_ready, 0, 0) < 2)
    Sleep(1);
  for (unsigned i = 0; i < 16; i++) {
    spu_channel_write(&c.ch_out_mbox, w->payload);
    if (spu_deliver_user_event(&c, w->port << 24) != 1 ||
        c.ch_in_mbox.count != 1 || spu_channel_read(&c.ch_in_mbox) != CELL_OK)
      w->failed++;
  }
}
#ifdef _WIN32
static DWORD WINAPI sender_entry(void *p) {
  sender_run(p);
  return 0;
}
#else
static void *sender_entry(void *p) {
  sender_run(p);
  return NULL;
}
#endif
static void concurrent_senders(uint32_t a, uint32_t b, uint32_t q1,
                               uint32_t q2) {
  attach(a, q1, 1, 0);
  attach(b, q2, 1, 0);
  Sender workers[2] = {{a, 1, 0xa1, 0}, {b, 1, 0xb1, 0}};
#ifdef _WIN32
  HANDLE threads[2];
  for (int i = 0; i < 2; i++) {
    threads[i] =
        CreateThread(NULL, 1u << 20, sender_entry, &workers[i], 0, NULL);
    CHECK(threads[i] != NULL);
  }
  for (int i = 0; i < 2; i++) {
    CHECK(WaitForSingleObject(threads[i], 5000) == WAIT_OBJECT_0);
    CloseHandle(threads[i]);
  }
#else
  pthread_t threads[2];
  for (int i = 0; i < 2; i++)
    CHECK(pthread_create(&threads[i], NULL, sender_entry, &workers[i]) == 0);
  for (int i = 0; i < 2; i++)
    CHECK(pthread_join(threads[i], NULL) == 0);
#endif
  CHECK(!workers[0].failed && !workers[1].failed);
  receive(q1, 16, 0xa1);
  receive(q2, 16, 0xb1);
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  vm_base = calloc(1, 0x10000);
  ppu_vm_size = 0x10000;
  CHECK(vm_base != NULL);
  uint32_t a = 0x2000, b = 0x4000, q1 = newq(), q2 = newq();
  const char *mode = argv[1];
  if (!strcmp(mode, "single")) {
    attach(a, q1, 1, 0);
    send(a, 1, 0xa1, 0);
    receive(q1, 1, 0xa1);
    receive(q2, 0, 0);
    send(a, 1, 0xa2, 1);
    receive(q1, 1, 0xa2);
  } else if (!strcmp(mode, "same-static") || !strcmp(mode, "same-dynamic")) {
    int dyn = !strcmp(mode, "same-dynamic");
    uint32_t p1 = attach(a, q1, 1, dyn), p2 = attach(b, q2, 1, dyn);
    CHECK(p1 == p2);
    send(b, p2, 0xb1, 0);
    receive(q1, 0, 0);
    receive(q2, 1, 0xb1);
  } else if (!strcmp(mode, "distinct-ports")) {
    attach(a, q1, 1, 0);
    attach(b, q2, 2, 0);
    send(a, 1, 0xa1, 0);
    send(b, 2, 0xb1, 0);
    receive(q1, 1, 0xa1);
    receive(q2, 1, 0xb1);
  } else if (!strcmp(mode, "reverse-order")) {
    attach(b, q2, 1, 0);
    attach(a, q1, 1, 0);
    send(a, 1, 0xa1, 0);
    receive(q2, 0, 0);
    receive(q1, 1, 0xa1);
  } else if (!strcmp(mode, "throw-collision")) {
    attach(a, q1, 1, 0);
    attach(b, q2, 1, 0);
    send(b, 1, 0xb1, 1);
    receive(q1, 0, 0);
    receive(q2, 1, 0xb1);
  } else if (!strcmp(mode, "duplicate-no-mutation")) {
    attach(a, q1, 1, 0);
    vm_base[0x100] = 1;
    CHECK(cellSpursAttachLv2EventQueue((CellSpurs *)(uintptr_t)a, q2,
                                       (u8 *)(uintptr_t)0x100,
                                       0) == CELL_SPURS_CORE_ERROR_BUSY);
    CHECK(s_spurs_event_queue_n == 1);
    send(a, 1, 0xa1, 0);
    receive(q1, 1, 0xa1);
    receive(q2, 0, 0);
  } else if (!strcmp(mode, "detach")) {
    attach(a, q1, 1, 0);
    attach(b, q2, 1, 0);
    CHECK(cellSpursDetachLv2EventQueue((CellSpurs *)(uintptr_t)0x6000, 1) == 0);
    CHECK(s_spurs_event_queue_n == 2);
    CHECK(cellSpursDetachLv2EventQueue((CellSpurs *)(uintptr_t)a, 1) == 0);
    send(b, 1, 0xb1, 0);
    receive(q1, 0, 0);
    receive(q2, 1, 0xb1);
  } else if (!strcmp(mode, "finalize-helper")) {
    attach(a, q1, 1, 0);
    attach(b, q2, 1, 0);
    spurs_cancel_attached_queues(a);
    CHECK(g_sys_event_queues[q1 - 1].cancelled == 1);
    CHECK(g_sys_event_queues[q2 - 1].cancelled == 0);
    CHECK(s_spurs_event_queue_n == 1);
    send(b, 1, 0xb1, 0);
    receive(q2, 1, 0xb1);
    attach(a, q1, 1, 0);
    CHECK(g_sys_event_queues[q1 - 1].cancelled == 0);
  } else if (!strcmp(mode, "finalized-sender")) {
    attach(a, q1, 1, 0);
    attach(b, q2, 1, 0);
    spurs_cancel_attached_queues(a);
    expected_ack = CELL_ENOTCONN;
    send(a, 1, 0xa1, 0);
    receive(q2, 0, 0);
  } else if (!strcmp(mode, "reuse-with-detach")) {
    attach(a, q1, 1, 0);
    CHECK(cellSpursDetachLv2EventQueue((CellSpurs *)(uintptr_t)a, 1) == 0);
    destroy(q1);
    uint32_t q3 = newq();
    CHECK(q3 == q1);
    CHECK(spurs_port_queue(a, 1) == 0);
    receive(q3, 0, 0);
  }

  else if (!strcmp(mode, "shared-queue-finalize")) {
    attach(a, q1, 1, 0);
    attach(b, q1, 2, 0);
    spurs_cancel_attached_queues(a);
    CHECK(port_binding(b, 2) >= 0);
    CHECK(g_sys_event_queues[q1 - 1].cancelled == 0);
  } else if (!strcmp(mode, "capacity")) {
    for (uint32_t i = 0; i < 8; i++)
      attach(a, q1, (uint8_t)i, 0);
    vm_base[0x100] = 1;
    CHECK(cellSpursAttachLv2EventQueue((CellSpurs *)(uintptr_t)b, q2,
                                       (u8 *)(uintptr_t)0x100, 0) == CELL_OK);
  } else if (!strcmp(mode, "unknown-owner")) {
    attach(a, q1, 1, 0);
    expected_ack = CELL_ENOTCONN;
    send(0x6000, 1, 0, 0);
    send(0x6000, 1, 0, 1);
    receive(q1, 0, 0);
  } else if (!strcmp(mode, "full-queue")) {
    attach(a, q1, 1, 0);
    for (unsigned i = 0; i < 16; i++)
      send(a, 1, 0xa1, 0);
    expected_ack = CELL_EBUSY;
    send(a, 1, 0xb1, 0);
    receive(q1, 16, 0xa1);
  } else if (!strcmp(mode, "malformed-mailbox")) {
    spu_context c;
    memset(&c, 0, sizeof c);
    c.spurs_ea = a;
    attach(a, q1, 1, 0);
    CHECK(spu_deliver_user_event(&c, 1u << 24) == 1);
    CHECK(spu_channel_read(&c.ch_in_mbox) == CELL_EINVAL);
    receive(q1, 0, 0);
  } else if (!strcmp(mode, "unowned-raw")) {
    spu_context c;
    memset(&c, 0, sizeof c);
    attach(a, q1, 1, 0);
    spu_channel_write(&c.ch_out_mbox, 0xbeef);
    CHECK(spu_deliver_user_event(&c, 1u << 24) == 0);
    CHECK(c.ch_in_mbox.count == 0 && c.ch_out_mbox.count == 1);
    CHECK(spu_channel_read(&c.ch_out_mbox) == 0xbeef);
    receive(q1, 0, 0);
  } else if (!strcmp(mode, "concurrent"))
    concurrent_senders(a, b, q1, q2);
  else
    return 2;
  printf("RESULT %s: %s (%d failed)\n", mode, failures ? "FAIL" : "PASS",
         failures);
  spurs_cancel_attached_queues(0);
  destroy(q1);
  destroy(q2);
  free(vm_base);
  return failures ? 1 : 0;
}
/* Unused blocking receive and workload-dispatch dependencies must fail loudly
 * if a future test accidentally enters them. */
void ydkj_release_pending_threads(void) { abort(); }
void ppu_thread_note_block(uint64_t id) {
  (void)id;
  abort();
}
void ppu_dump_guest_stack(ppu_context *c, const char *t) {
  (void)c;
  (void)t;
  abort();
}
uint32_t spu_elf_image_size(const uint8_t *p) {
  (void)p;
  abort();
  return 0;
}
int spu_workload_dispatch_async(const uint8_t *p, uint32_t n, uint32_t a) {
  (void)p;
  (void)n;
  (void)a;
  abort();
  return 0;
}
int spu_dispatch_frame_by_queue(uint32_t q, uint32_t t) {
  (void)q;
  (void)t;
  abort();
  return 0;
}
int64_t lv2_usec_deadline(uint64_t u) {
  (void)u;
  abort();
  return 0;
}
int lv2_deadline_passed(int64_t d) {
  (void)d;
  abort();
  return 0;
}
uint32_t g_spu_pending_evt[3];
int g_spu_pending_evt_valid;

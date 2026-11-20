#include "rsx_gpu_wait.h"

#include <stdio.h>

static int failures;

static void check(int condition, const char* name)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

typedef struct {
    unsigned signal_after;   /* the wait succeeds on this call (1-based); 0 = never */
    unsigned removed_after;  /* the device reads as removed from this call on; 0 = never */
    unsigned waits, removed_checks, timeouts_seen;
    int last_removed_seen;
} Fake;

static int f_wait(void* c)
{
    Fake* f = (Fake*)c;
    f->waits++;
    return f->signal_after && f->waits >= f->signal_after;
}
static int f_removed(void* c)
{
    Fake* f = (Fake*)c;
    f->removed_checks++;
    return f->removed_after && f->waits >= f->removed_after;
}
static void f_timeout(void* c, unsigned n, int removed)
{
    Fake* f = (Fake*)c;
    f->timeouts_seen = n;
    f->last_removed_seen = removed;
}

int main(void)
{
    RsxGpuWaitOps ops = { f_wait, f_removed, f_timeout };

    Fake a = { 1, 0, 0, 0, 0, 0 };
    check(rsx_gpu_wait_alive(&ops, &a) == 1 && a.waits == 1 && a.removed_checks == 0,
          "an immediate signal returns at once without asking about the device");

    /* A slow but alive GPU: far past the three timeouts the old code gave up after. */
    Fake b = { 50, 0, 0, 0, 0, 0 };
    check(rsx_gpu_wait_alive(&ops, &b) == 1, "a slow GPU is waited for until the fence is reached");
    check(b.waits == 50 && b.timeouts_seen == 49 && !b.last_removed_seen,
          "every timeout on a live device is reported and the wait goes on");

    /* A removed device: given up on at the first timeout that sees the removal. */
    Fake c = { 0, 4, 0, 0, 0, 0 };
    check(rsx_gpu_wait_alive(&ops, &c) == 0, "a removed device is given up on");
    check(c.waits == 4 && c.last_removed_seen, "the removal is acted on as soon as it is seen");

    RsxGpuWaitOps quiet = { f_wait, f_removed, NULL };
    Fake d = { 3, 0, 0, 0, 0, 0 };
    check(rsx_gpu_wait_alive(&quiet, &d) == 1, "the timeout callback is optional");

    return failures ? 1 : 0;
}

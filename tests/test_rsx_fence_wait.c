#include "rsx_fence_wait.h"

#include <stdio.h>

static int failures;

static void check(int condition, const char* name)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

int main(void)
{
    check(rsx_fence_ready(1, 7, 7),
          "the required fence is complete");
    check(rsx_fence_ready(1, 8, 7),
          "a later fence also satisfies the wait");
    check(!rsx_fence_ready(1, 6, 7),
          "an event cannot release resources before the required fence");
    check(!rsx_fence_ready(0, 7, 7),
          "a failed wait cannot release resources");
    check(!rsx_fence_ready(1, UINT64_MAX, 7),
          "device removal cannot be mistaken for completion");
    check(!rsx_fence_ready(1, UINT64_MAX, 0),
          "device removal is not completion even for fence zero");
    return failures ? 1 : 0;
}

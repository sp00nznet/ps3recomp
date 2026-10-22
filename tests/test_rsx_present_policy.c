#include "rsx_present_policy.h"

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
    check(!rsx_present_flip_already_rendered(0, 0, 0),
          "boot fallback remains active before a clear present");
    check(rsx_present_flip_already_rendered(1, 7, 7),
          "the flip rendered by a clear boundary is not rendered twice");
    check(!rsx_present_flip_already_rendered(1, 7, 8),
          "a later guest flip resumes normal presentation");
    check(rsx_present_flip_already_rendered(1, 0, 0),
          "a pre-flip clear present suppresses the duplicate boot present");
    return failures ? 1 : 0;
}

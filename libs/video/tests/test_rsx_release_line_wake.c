/*
 * ps3recomp - PS3RECOMP_RSX_RELEASE_LINE_WAKE
 *
 * An NV4097 semaphore release into main memory (context DMA 0xFEED0001)
 * calls g_rsx_main_release_hook with the 128-byte line it wrote, after the
 * write, when PS3RECOMP_RSX_RELEASE_LINE_WAKE=1. Unset, the hook is never
 * called. Label-window releases (any other context) never call it.
 *
 *   cc -std=gnu17 -I include -I libs/video libs/video/tests/test_rsx_release_line_wake.c \
 *      libs/video/rsx_commands.c -o /tmp/test_rsx_release_line_wake
 *   /tmp/test_rsx_release_line_wake off && /tmp/test_rsx_release_line_wake on
 * (one process per mode: the variable is read once).
 */
#include "rsx_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t ppu_hle_inject_base;
void ps3_ms(const char* key) { (void)key; }
uint32_t gcm_io_offset_to_ea(uint32_t io) { return 0x10000000u + io; }
unsigned long long ps3_ms_now(void) { return 1234; }
extern void (*g_rsx_main_release_hook)(uint32_t line);

static u32 wr_addr[16], wr_val[16], wr_n;
void vm_write32(uint32_t addr, uint32_t value)
{
    if (wr_n < 16) { wr_addr[wr_n] = addr; wr_val[wr_n] = value; }
    wr_n++;
}

/* The hook sees the line, and the value is already in memory when it runs. */
static u32 hook_line[16], hook_writes_before[16], hook_n;
static void hook(uint32_t line)
{
    if (hook_n < 16) { hook_line[hook_n] = line; hook_writes_before[hook_n] = wr_n; }
    hook_n++;
}

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

int main(int argc, char** argv)
{
    const int on = argc > 1 && !strcmp(argv[1], "on");
    if (on) setenv("PS3RECOMP_RSX_RELEASE_LINE_WAKE", "1", 1);
    else unsetenv("PS3RECOMP_RSX_RELEASE_LINE_WAKE");
    g_rsx_main_release_hook = hook;

    static rsx_state st;
    rsx_state_init(&st);

    /* Main memory: IO offset 0x1234C lands at 0x1001234C, line 0x10012300. */
    rsx_process_method(&st, 0x01A4u, 0xFEED0001u);
    rsx_process_method(&st, 0x1D6Cu, 0x0001234Cu);
    rsx_process_method(&st, 0x1D74u, 0x00000005u);
    check(wr_n == 1 && wr_addr[0] == 0x1001234Cu && wr_val[0] == 5u,
          "the release writes main memory");
    if (on)
        check(hook_n == 1 && hook_line[0] == 0x10012300u && hook_writes_before[0] == 1,
              "on: the hook gets the written line, after the write");
    else
        check(hook_n == 0, "off: the hook is not called");

    rsx_process_method(&st, 0x1D70u, 0x00010000u);   /* back-end release: byte-swapped value */
    check(wr_n == 2 && wr_val[1] == 1u, "the back-end release writes main memory");
    check(hook_n == (on ? 2u : 0u), "and calls the hook only when on");

    /* A label-window release is not a main-memory line. */
    rsx_process_method(&st, 0x01A4u, 0xFEED0000u);
    rsx_process_method(&st, 0x1D74u, 0x00000007u);
    check(wr_n == 3, "the label-window release writes");
    check(hook_n == (on ? 2u : 0u), "a label-window release never calls the hook");

    printf("[rsx_release_line_wake %s] %s (%d failed)\n", on ? "on" : "off",
           fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

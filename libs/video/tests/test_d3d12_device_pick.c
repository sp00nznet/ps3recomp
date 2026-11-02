/*
 * Standalone unit test for the D3D12 device order in rsx_d3d12_device.h
 * (rsx_pick_device and RSX_D3D12_WARP parsing). Pure C, no D3D12: the
 * Windows half of the header is not compiled off Windows.
 *
 * Build, from the repository root:
 *   cc -std=gnu17 -Wall -Wextra -I include -I libs/video \
 *       -o test_d3d12_device_pick libs/video/tests/test_d3d12_device_pick.c
 *
 * Each case gives an adapter list and which creation attempts succeed, then
 * checks the attempt sequence and the result.
 */

#include "rsx_d3d12_device.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

static void check_true(const char* test, int cond)
{
    if (cond) { printf("[PASS] %s\n", test); g_pass++; }
    else      { printf("[FAIL] %s\n", test); g_fail++; }
}

/* Fake device creation: ok_mask bit i = adapter i works, ok_default and
 * ok_warp the other two; every attempt is recorded. */
typedef struct {
    unsigned ok_mask;
    int ok_default, ok_warp;
    int tried[32];
    int ntried;
} fake;

static int fake_try(void* vctx, int which)
{
    fake* f = (fake*)vctx;
    if (f->ntried < 32) f->tried[f->ntried++] = which;
    if (which >= 0) return (f->ok_mask >> which) & 1u;
    if (which == RSX_PICK_DEFAULT) return f->ok_default;
    if (which == RSX_PICK_WARP) return f->ok_warp;
    return 0;
}

static int same_seq(const fake* f, const int* want, int n)
{
    if (f->ntried != n) return 0;
    for (int i = 0; i < n; i++)
        if (f->tried[i] != want[i]) return 0;
    return 1;
}

#define HW(v, d)   { (v), (d), 0 }
#define SWFLAG     { 0x1234u, 0x5678u, 1 }
#define BASIC_RD   { RSX_WARP_VENDOR, RSX_WARP_DEVICE, 0 }   /* WARP without the flag */

static void run(const char* name, int mode, const rsx_adapter_info* a, int n,
                unsigned ok_mask, int ok_default, int ok_warp,
                int want_result, const int* want_seq, int want_n)
{
    fake f;
    memset(&f, 0, sizeof f);
    f.ok_mask = ok_mask; f.ok_default = ok_default; f.ok_warp = ok_warp;
    int r = rsx_pick_device(mode, a, n, fake_try, &f);
    char buf[128];
    snprintf(buf, sizeof buf, "%s_result", name);
    check_true(buf, r == want_result);
    snprintf(buf, sizeof buf, "%s_attempts", name);
    check_true(buf, same_seq(&f, want_seq, want_n));
}

int main(void)
{
    /* RSX_D3D12_WARP parsing. */
    {
        int bad = -1;
        check_true("parse_unset",  rsx_warp_mode_parse(NULL, &bad) == RSX_WARP_AUTO && bad == 0);
        check_true("parse_empty",  rsx_warp_mode_parse("", &bad) == RSX_WARP_AUTO && bad == 0);
        check_true("parse_1",      rsx_warp_mode_parse("1", &bad) == RSX_WARP_FORCE && bad == 0);
        check_true("parse_0",      rsx_warp_mode_parse("0", &bad) == RSX_WARP_NEVER && bad == 0);
        check_true("parse_10_bad", rsx_warp_mode_parse("10", &bad) == RSX_WARP_AUTO && bad == 1);
        check_true("parse_yes_bad", rsx_warp_mode_parse("yes", &bad) == RSX_WARP_AUTO && bad == 1);
        check_true("parse_null_bad_ptr", rsx_warp_mode_parse("1", NULL) == RSX_WARP_FORCE);
    }

    /* Software adapters: by the flag, or by the Basic Render Driver's ids. */
    check_true("sw_flag",      rsx_adapter_is_software(1, 0x10DEu, 0x2860u));
    check_true("sw_basic_ids", rsx_adapter_is_software(0, 0x1414u, 0x008Cu));
    check_true("sw_other_ms",  !rsx_adapter_is_software(0, 0x1414u, 0x0001u));
    check_true("sw_hw",        !rsx_adapter_is_software(0, 0x10DEu, 0x2860u));

    /* The first hardware adapter in preference order works: nothing else is tried. */
    {
        const rsx_adapter_info a[] = { HW(0x10DEu, 0x2860u), HW(0x8086u, 0x7D55u) };
        const int seq[] = { 0 };
        run("first_hw", RSX_WARP_AUTO, a, 2, 0x3u, 1, 1, 0, seq, 1);
    }
    /* A failing adapter is passed over for the next. */
    {
        const rsx_adapter_info a[] = { HW(0x10DEu, 0x2860u), HW(0x8086u, 0x7D55u) };
        const int seq[] = { 0, 1 };
        run("second_hw", RSX_WARP_AUTO, a, 2, 0x2u, 1, 1, 1, seq, 2);
    }
    /* Software adapters are never tried as hardware, flagged or not. */
    {
        const rsx_adapter_info a[] = { SWFLAG, BASIC_RD, HW(0x1002u, 0x744Cu) };
        const int seq[] = { 2 };
        run("skip_software", RSX_WARP_AUTO, a, 3, 0x7u, 1, 1, 2, seq, 1);
    }
    /* No hardware adapter works: the default adapter. */
    {
        const rsx_adapter_info a[] = { HW(0x10DEu, 0x2860u) };
        const int seq[] = { 0, RSX_PICK_DEFAULT };
        run("default", RSX_WARP_AUTO, a, 1, 0x0u, 1, 1, RSX_PICK_DEFAULT, seq, 2);
    }
    /* An empty list (or no IDXGIFactory6 adapters): the default adapter. */
    {
        const int seq[] = { RSX_PICK_DEFAULT };
        run("empty_list", RSX_WARP_AUTO, NULL, 0, 0x0u, 1, 1, RSX_PICK_DEFAULT, seq, 1);
    }
    /* Only the Basic Render Driver listed, default adapter fails: WARP. */
    {
        const rsx_adapter_info a[] = { BASIC_RD };
        const int seq[] = { RSX_PICK_DEFAULT, RSX_PICK_WARP };
        run("warp_fallback", RSX_WARP_AUTO, a, 1, 0x1u, 0, 1, RSX_PICK_WARP, seq, 2);
    }
    /* Nothing works at all. */
    {
        const rsx_adapter_info a[] = { HW(0x10DEu, 0x2860u) };
        const int seq[] = { 0, RSX_PICK_DEFAULT, RSX_PICK_WARP };
        run("none", RSX_WARP_AUTO, a, 1, 0x0u, 0, 0, RSX_PICK_NONE, seq, 3);
    }
    /* RSX_D3D12_WARP=1: WARP only, even with a working GPU. */
    {
        const rsx_adapter_info a[] = { HW(0x10DEu, 0x2860u) };
        const int seq[] = { RSX_PICK_WARP };
        run("force_warp", RSX_WARP_FORCE, a, 1, 0x1u, 1, 1, RSX_PICK_WARP, seq, 1);
    }
    /* RSX_D3D12_WARP=0: no WARP when everything else fails. */
    {
        const rsx_adapter_info a[] = { HW(0x10DEu, 0x2860u) };
        const int seq[] = { 0, RSX_PICK_DEFAULT };
        run("never_warp", RSX_WARP_NEVER, a, 1, 0x0u, 0, 1, RSX_PICK_NONE, seq, 2);
    }
    /* RSX_D3D12_WARP=0 changes nothing when the hardware works. */
    {
        const rsx_adapter_info a[] = { HW(0x10DEu, 0x2860u) };
        const int seq[] = { 0 };
        run("never_warp_hw", RSX_WARP_NEVER, a, 1, 0x1u, 1, 1, 0, seq, 1);
    }

    printf("\n===========================================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/*
 * ps3recomp - SET_TRANSFORM_CONSTANT window test
 *
 * NV4097_SET_TRANSFORM_CONSTANT is a 32-dword window, 0x1F00..0x1F7C (eight
 * vec4s starting at TRANSFORM_CONSTANT_LOAD). The methods after it are
 * registers, not more constants: 0x1FD8 INVALIDATE_L2, 0x1FF0
 * VERTEX_ATTRIB_INPUT_MASK, 0x1FF8 TRANSFORM_BRANCH_BITS. Taking the window as
 * 64 dwords made each of those overwrite a constant c(load+8..15).
 *
 * Build (standalone):
 *   cc -std=c11 -I include -I libs/video libs/video/tests/test_rsx_transform_constants.c \
 *      libs/video/rsx_commands.c -o /tmp/test_rsx_transform_constants
 */
#include "rsx_commands.h"

#include <stdio.h>
#include <string.h>

/* rsx_commands.c reaches the guest VM (semaphore labels) and the boot
 * milestone log; nothing here uses either, so stubs are enough to link. */
uint32_t ppu_hle_inject_base;
void vm_write32(uint32_t addr, uint32_t value) { (void)addr; (void)value; }
void ps3_ms(const char* key) { (void)key; }

static int g_fail;

static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail = 1;
}

static u32 fbits(float f)
{
    u32 b;
    memcpy(&b, &f, 4);
    return b;
}

int main(void)
{
    rsx_state st;
    rsx_state_init(&st);

    const float k269 = 3.5f, k271 = -7.25f, w263 = 42.0f;

    rsx_process_method(&st, NV4097_SET_TRANSFORM_CONSTANT_LOAD, 269);
    rsx_process_method(&st, NV4097_SET_TRANSFORM_CONSTANT + 2 * 4, fbits(k269));   /* c269.z */
    rsx_process_method(&st, NV4097_SET_TRANSFORM_CONSTANT_LOAD, 271);
    rsx_process_method(&st, NV4097_SET_TRANSFORM_CONSTANT + 0 * 4, fbits(k271));   /* c271.x */
    check(st.vertex_constants[269][2] == k269 && st.vertex_constants[271][0] == k271,
          "constants seeded through the 0x1F00 window");

    /* load = 256: the old 64-dword window put 0x1FD8 on c269.z and 0x1FF0 on c271.x */
    rsx_process_method(&st, NV4097_SET_TRANSFORM_CONSTANT_LOAD, 256);
    check(rsx_process_method(&st, NV4097_INVALIDATE_L2, 0x12345678u) == 0,
          "INVALIDATE_L2 (0x1FD8) accepted");
    check(rsx_process_method(&st, NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, 0x0000FFFFu) == 0,
          "VERTEX_ATTRIB_INPUT_MASK (0x1FF0) accepted");
    check(st.vertex_constants[269][2] == k269, "0x1FD8 with load=256 leaves c269.z alone");
    check(st.vertex_constants[271][0] == k271, "0x1FF0 with load=256 leaves c271.x alone");
    check(st.vertex_attrib_input_mask == 0x0000FFFFu, "0x1FF0 lands in vertex_attrib_input_mask");

    rsx_process_method(&st, 0x00001FC0u, 0x3u);
    rsx_process_method(&st, NV4097_SET_TRANSFORM_BRANCH_BITS, 0x81u);
    check(st.frequency_divider_op == 0x3u && st.transform_branch_bits == 0x81u,
          "0x1FC0/0x1FF8 land in their registers");

    rsx_process_method(&st, NV4097_SET_TRANSFORM_CONSTANT + 31 * 4, fbits(w263));  /* 0x1F7C */
    check(st.vertex_constants[263][3] == w263, "0x1F7C (last dword) still writes c263.w");

    printf("%s\n", g_fail ? "test_rsx_transform_constants: FAIL" : "test_rsx_transform_constants: PASS");
    return g_fail;
}

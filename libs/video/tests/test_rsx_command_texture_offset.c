/*
 * ps3recomp - texture offsets are 31 bits in the command processor
 *
 * NV4097_SET_TEXTURE_OFFSET (0x1A00 + unit*0x20) and the vertex texture offset
 * (0x0900 + unit*0x20) carry a 31-bit offset; bit 31 is not an address bit.
 * rsx_process_method stored the word whole, so a bound offset of 0x82A22900
 * reached the backend with bit 31 set.
 *
 *   gcc -std=gnu17 -Wall -Wextra -I include -I libs/video \
 *       -o /tmp/test_rsx_command_texture_offset \
 *       libs/video/tests/test_rsx_command_texture_offset.c libs/video/rsx_commands.c
 */
#include "rsx_commands.h"

#include <stdint.h>
#include <stdio.h>

/* rsx_commands.c reaches the guest VM and the boot milestone log; stubs are enough to link. */
uint32_t ppu_hle_inject_base;
void vm_write32(uint32_t addr, uint32_t value) { (void)addr; (void)value; }
void ps3_ms(const char* key) { (void)key; }
uint32_t gcm_io_offset_to_ea(uint32_t io) { (void)io; return 0; }
unsigned long long ps3_ms_now(void) { return 0; }

static int g_failures;

static void check(int ok, const char* what, unsigned got)
{
    printf("[%s] %s (got 0x%08X)\n", ok ? "PASS" : "FAIL", what, got);
    if (!ok) g_failures++;
}

int main(void)
{
    rsx_state st;
    rsx_state_init(&st);

    rsx_process_method(&st, 0x1A00 + 2 * 0x20, 0x82A22900u);
    check(st.textures[2].offset == 0x02A22900u, "fragment unit 2 offset 0x82A22900 stores as 0x02A22900", st.textures[2].offset);

    rsx_process_method(&st, 0x1A00, 0x0438000u);
    check(st.textures[0].offset == 0x0438000u, "fragment unit 0 offset 0x00438000 unchanged", st.textures[0].offset);

    rsx_process_method(&st, 0x0900 + 1 * 0x20, 0x80100000u);
    check(st.vertex_textures[1].offset == 0x00100000u, "vertex unit 1 offset 0x80100000 stores as 0x00100000", st.vertex_textures[1].offset);

    if (g_failures) { printf("%d check(s) FAILED\n", g_failures); return 1; }
    printf("all checks passed\n");
    return 0;
}

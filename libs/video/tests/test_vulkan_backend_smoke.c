/*
 * Bring the Vulkan RSX backend up, present a few frames and shut it down,
 * twice: the platform layer (loader, window, message pump, mutexes, clock)
 * on its own, with no command stream. No game files are involved.
 *
 * Headless by default; PS3RECOMP_VK_WINDOW=1 adds the window (Win32 on
 * Windows, SDL2 elsewhere) and checks the pump keeps it open. Exit 0 on
 * success, 1 on a failed check, 77 when no Vulkan loader or device exists.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "rsx_vulkan_backend.h"

uint8_t* vm_base = NULL;   /* the backend reads guest memory only for draws */

static int g_failures;
#define CHECK(cond, ...) do { \
    if (cond) printf("[PASS] " __VA_ARGS__); else { printf("[FAIL] " __VA_ARGS__); g_failures++; } \
    printf("\n"); } while (0)

static int one_session(int round)
{
    if (rsx_vulkan_backend_init(64, 64, "ps3recomp Vulkan smoke test") != 0) return -1;
    u32 shown = 0;
    for (int frame = 0; frame < 3; frame++) {
        CHECK(rsx_vulkan_backend_pump_messages() == 0,
              "round %d frame %d: the window (if any) stays open", round, frame);
        rsx_vulkan_backend_present();
        shown = rsx_vulkan_backend_readback_center();
    }
    CHECK((shown >> 24) == 0xFFu,
          "round %d: a present read the frame back (centre 0x%08X)", round, (unsigned)shown);
    rsx_vulkan_backend_shutdown();
    CHECK(rsx_vulkan_backend_readback_center() == 0,
          "round %d: shutdown clears the backend's state", round);
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = (uint8_t*)calloc(1, 1u << 20);
    if (!vm_base) return 1;
    for (int round = 0; round < 2; round++) {
        if (one_session(round) != 0) {
            if (round == 0) {
                printf("SKIP: no usable Vulkan loader or device here\n");
                free(vm_base);
                return 77;
            }
            CHECK(0, "round %d: the backend comes up again after a shutdown", round);
        }
    }
    free(vm_base);
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}

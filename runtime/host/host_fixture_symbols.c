/* Game-free renderer fixture only. Never link this into a game executable. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

const uint64_t function_table_count = 0;
const struct { uint64_t addr; void* func; const char* name; } function_table[1] = {{0, 0, 0}};

int ppu_callback_dispatch_active(void) { return 0; }

void ppu_callback_worker_guest_exit(void)
{
    fputs("Renderer fixture unexpectedly entered a guest callback\n", stderr);
    abort();
}

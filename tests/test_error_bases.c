/* One literal per module error base, so a base cannot drift unnoticed again. Each value is the console's (SDK
 * headers), not derived from the macros under test: CELL_ERROR_BASE_GCM was 0x80210700 and every CELL_GCM_ERROR_* came
 * out 0x802107xx; the SPURS task codes were built on the core base (0x804107xx) instead of 0x804109xx. */
#include <stdio.h>
#include <stdlib.h>
#include "ps3emu/error_codes.h"
#include "../libs/spurs/cellSpurs.h"

static int fails;
#define EXPECT(name, value) do { if ((uint32_t)(name) != (uint32_t)(value)) { \
    fprintf(stderr, "FAIL: %s = 0x%08X, want 0x%08X\n", #name, (unsigned)(uint32_t)(name), (unsigned)(value)); fails++; } } while (0)

int main(void)
{
    EXPECT(CELL_GCM_ERROR_NO_IO_PAGE_TABLE,  0x80210001u);
    EXPECT(CELL_GCM_ERROR_INVALID_ALIGNMENT, 0x80210004u);
    EXPECT(CELL_GCM_ERROR_FAILURE,           0x802100FFu);
    EXPECT(CELL_SPURS_CORE_ERROR_AGAIN,      0x80410701u);
    EXPECT(CELL_SPURS_TASK_ERROR_AGAIN,      0x80410901u);
    EXPECT(CELL_SPURS_TASK_ERROR_NOENT,      0x80410906u);
    EXPECT(CELL_SPURS_TASK_ERROR_NULL_POINTER, 0x80410911u);
    EXPECT(CELL_AUDIO_ERROR_ALREADY_INIT,    0x80310701u);
    EXPECT(CELL_PAD_ERROR_FATAL,             0x80121101u);
    if (fails) return 1;
    puts("PASS: GCM, SPURS core and task, audio and pad error bases");
    return 0;
}

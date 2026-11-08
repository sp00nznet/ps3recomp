/* A second translation unit for test_rsx_fifo_busy.c: the header-only state
 * must be one object across units, as it is across cellGcmSys.c, the draw
 * engine and the backends. */
#include "../rsx_fifo_busy.h"

void fifo_busy_tu2_enter(int category) { rsx_fifo_busy_enter(category); }
void fifo_busy_tu2_leave(int category) { rsx_fifo_busy_leave(category); }
const void* fifo_busy_tu2_state(void) { return &g_rsx_fifo_busy; }

#include "ppu_host_unwind.h"
#if defined(_MSC_VER)
#define PPU_UNWIND_TLS __declspec(thread)
#else
#define PPU_UNWIND_TLS _Thread_local
#endif
static PPU_UNWIND_TLS void (*s_handler)(void);
void ps3_ppu_set_host_unwind_handler(void (*handler)(void)) { s_handler = handler; }
void ps3_ppu_notify_host_unwind(void) { if (s_handler) s_handler(); }

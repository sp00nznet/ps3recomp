/* Unused dependency traps for the CPU-only GCM fixture on COFF linkers.
 * No renderer operation is permitted: reaching any stub aborts the test. */
#include "../libs/video/rsx_live_draw.h"
#include "../libs/video/rsx_draw_engine.h"
#include "../libs/video/rsx_commands.h"
#include "../runtime/ppu/ppu_context.h"
#include <stdio.h>
#include <stdlib.h>
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(u32,u32);
static _Noreturn void unexpected(const char* name)
{ fprintf(stderr,"GCM CPU fixture reached unrelated dependency: %s\n",name);abort(); }
#define TRAP() unexpected(__func__)
void rsx_state_init(rsx_state* s) { TRAP(); }
int rsx_draw_engine_enabled(void) { TRAP(); }
int rsx_live_draw_enabled(void) { TRAP(); }
void rsx_draw_engine_note_flip(u32 a) { TRAP(); }
void rsx_raise_user_cmd(u32 a) { TRAP(); }
void rsx_live_draw_method(u32 a,u32 b) { TRAP(); }
void rsx_draw_engine_method(u32 a,u32 b) { TRAP(); }
void rsx_draw_engine_flush(void) { TRAP(); }
int rsx_process_method(rsx_state* s,u32 a,u32 b) { TRAP(); }
void rsx_draw_engine_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { TRAP(); }
void rsx_live_draw_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { TRAP(); }
void ppu_dump_guest_stack(ppu_context* c,const char* s) { TRAP(); }
void ppu_dump_bctrl_ring(uint32_t a,const char* s) { TRAP(); }
int rsx_live_draw_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h) { TRAP(); }
int rsx_live_draw_resolve_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h,u32 i,u32 j,u32 k,u32 l) { TRAP(); }
void rsx_live_draw_note_resolve(u32 a,u32 b,u32 c,u32 d) { TRAP(); }
void rsx_draw_engine_guest_read(u32 a,u32 b,u32 c,u32 d,u32 e) { TRAP(); }
void rsx_draw_engine_guest_wrote(u32 a,u32 b,u32 c,u32 d,u32 e) { TRAP(); }

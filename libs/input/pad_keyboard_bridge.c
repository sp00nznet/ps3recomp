#include "pad_keyboard_bridge.h"
#include "pad_keyboard_cache.h"

static PadKeyboardCache s_keyboard;
void ps3_pad_keyboard_attach(uint32_t window_id)
{ pad_keyboard_cache_attach(&s_keyboard,window_id); }
void ps3_pad_keyboard_detach(void)
{ pad_keyboard_cache_detach(&s_keyboard); }
void ps3_pad_keyboard_focus(uint32_t window_id,int focused)
{ pad_keyboard_cache_focus(&s_keyboard,window_id,focused); }
void ps3_pad_keyboard_key(uint32_t window_id,uint16_t button,int pressed,int repeat)
{ pad_keyboard_cache_key(&s_keyboard,window_id,button,pressed,repeat); }
uint32_t ps3_pad_keyboard_read(void)
{ return pad_keyboard_cache_read(&s_keyboard); }

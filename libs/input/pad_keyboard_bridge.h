#ifndef PS3_PAD_KEYBOARD_BRIDGE_H
#define PS3_PAD_KEYBOARD_BRIDGE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* UI thread publishes actual window events. Guest threads only read. */
void ps3_pad_keyboard_attach(uint32_t window_id);
void ps3_pad_keyboard_detach(void);
void ps3_pad_keyboard_focus(uint32_t window_id, int focused);
void ps3_pad_keyboard_key(uint32_t window_id, uint16_t button, int pressed, int repeat);
uint32_t ps3_pad_keyboard_read(void);
#ifdef __cplusplus
}
#endif
#endif

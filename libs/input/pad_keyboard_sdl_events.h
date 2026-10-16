#ifndef PS3_PAD_KEYBOARD_SDL_EVENTS_H
#define PS3_PAD_KEYBOARD_SDL_EVENTS_H
#include <SDL2/SDL.h>
#include "cellPad.h"
#include "pad_keyboard_bridge.h"

/* Called only from the SDL window's existing UI event pump. */
static inline void ps3_pad_keyboard_sdl_event(const SDL_Event* e,uint32_t window_id)
{
    if (!e || !window_id) return;
    if (e->type==SDL_QUIT) { ps3_pad_keyboard_detach();return; }
    if (e->type==SDL_APP_WILLENTERBACKGROUND) {
        ps3_pad_keyboard_focus(window_id,0);return;
    }
    if (e->type==SDL_WINDOWEVENT) {
        switch(e->window.event) {
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            ps3_pad_keyboard_focus(e->window.windowID,1);break;
        case SDL_WINDOWEVENT_FOCUS_LOST:
        case SDL_WINDOWEVENT_HIDDEN:
        case SDL_WINDOWEVENT_MINIMIZED:
        case SDL_WINDOWEVENT_CLOSE:
            ps3_pad_keyboard_focus(e->window.windowID,0);break;
        default:break;
        }
        return;
    }
    if (e->type!=SDL_KEYDOWN && e->type!=SDL_KEYUP) return;
    uint16_t button=0;
    switch(e->key.keysym.scancode) {
    case SDL_SCANCODE_UP:button=CELL_PAD_CTRL_UP;break;
    case SDL_SCANCODE_DOWN:button=CELL_PAD_CTRL_DOWN;break;
    case SDL_SCANCODE_LEFT:button=CELL_PAD_CTRL_LEFT;break;
    case SDL_SCANCODE_RIGHT:button=CELL_PAD_CTRL_RIGHT;break;
    case SDL_SCANCODE_Z:button=CELL_PAD_CTRL_CROSS;break;
    case SDL_SCANCODE_X:button=CELL_PAD_CTRL_CIRCLE;break;
    case SDL_SCANCODE_A:button=CELL_PAD_CTRL_SQUARE;break;
    case SDL_SCANCODE_S:button=CELL_PAD_CTRL_TRIANGLE;break;
    case SDL_SCANCODE_Q:button=CELL_PAD_CTRL_L1;break;
    case SDL_SCANCODE_W:button=CELL_PAD_CTRL_R1;break;
    case SDL_SCANCODE_1:button=CELL_PAD_CTRL_L2;break;
    case SDL_SCANCODE_2:button=CELL_PAD_CTRL_R2;break;
    case SDL_SCANCODE_RETURN:button=CELL_PAD_CTRL_START;break;
    case SDL_SCANCODE_TAB:button=CELL_PAD_CTRL_SELECT;break;
    default:break;
    }
    if (button) ps3_pad_keyboard_key(e->key.windowID,button,e->type==SDL_KEYDOWN,!!e->key.repeat);
}
#endif

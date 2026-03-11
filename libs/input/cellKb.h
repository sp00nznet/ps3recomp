/*
 * ps3recomp - cellKb HLE
 *
 * Keyboard input: initialization, reading key state.
 */

#ifndef PS3RECOMP_CELL_KB_H
#define PS3RECOMP_CELL_KB_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/

#define CELL_KB_ERROR_FATAL                 (CELL_ERROR_BASE_KB | 0x01)
#define CELL_KB_ERROR_INVALID_PARAMETER     (CELL_ERROR_BASE_KB | 0x02)
#define CELL_KB_ERROR_ALREADY_INITIALIZED   (CELL_ERROR_BASE_KB | 0x03)
#define CELL_KB_ERROR_UNINITIALIZED         (CELL_ERROR_BASE_KB | 0x04)
#define CELL_KB_ERROR_NO_DEVICE             (CELL_ERROR_BASE_KB | 0x05)

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define CELL_KB_MAX_KEYBOARDS       2
#define CELL_KB_MAX_KEYCODES        62

/* Read modes */
#define CELL_KB_RMODE_INPUTCHAR     0  /* Cooked: characters after key repeat/mapping */
#define CELL_KB_RMODE_PACKET        1  /* Raw: every make/break as a packet */

/* Code types */
#define CELL_KB_CODETYPE_RAW        0  /* USB HID scan codes */
#define CELL_KB_CODETYPE_ASCII      1  /* ASCII-mapped codes */

/* Modifier key flags (in mkey field) */
#define CELL_KB_MKEY_L_CTRL         (1 << 0)
#define CELL_KB_MKEY_L_SHIFT        (1 << 1)
#define CELL_KB_MKEY_L_ALT          (1 << 2)
#define CELL_KB_MKEY_L_WIN          (1 << 3)
#define CELL_KB_MKEY_R_CTRL         (1 << 4)
#define CELL_KB_MKEY_R_SHIFT        (1 << 5)
#define CELL_KB_MKEY_R_ALT          (1 << 6)
#define CELL_KB_MKEY_R_WIN          (1 << 7)

/* LED flags */
#define CELL_KB_LED_NUM_LOCK        (1 << 0)
#define CELL_KB_LED_CAPS_LOCK       (1 << 1)
#define CELL_KB_LED_SCROLL_LOCK     (1 << 2)
#define CELL_KB_LED_COMPOSE         (1 << 3)
#define CELL_KB_LED_KANA            (1 << 4)

/* PS3 raw keycodes (subset - USB HID usage page 0x07) */
#define CELL_KEYC_NO_EVENT          0x00
#define CELL_KEYC_A                 0x04
#define CELL_KEYC_B                 0x05
#define CELL_KEYC_C                 0x06
#define CELL_KEYC_D                 0x07
#define CELL_KEYC_E                 0x08
#define CELL_KEYC_F                 0x09
#define CELL_KEYC_G                 0x0A
#define CELL_KEYC_H                 0x0B
#define CELL_KEYC_I                 0x0C
#define CELL_KEYC_J                 0x0D
#define CELL_KEYC_K                 0x0E
#define CELL_KEYC_L                 0x0F
#define CELL_KEYC_M                 0x10
#define CELL_KEYC_N                 0x11
#define CELL_KEYC_O                 0x12
#define CELL_KEYC_P                 0x13
#define CELL_KEYC_Q                 0x14
#define CELL_KEYC_R                 0x15
#define CELL_KEYC_S                 0x16
#define CELL_KEYC_T                 0x17
#define CELL_KEYC_U                 0x18
#define CELL_KEYC_V                 0x19
#define CELL_KEYC_W                 0x1A
#define CELL_KEYC_X                 0x1B
#define CELL_KEYC_Y                 0x1C
#define CELL_KEYC_Z                 0x1D
#define CELL_KEYC_1                 0x1E
#define CELL_KEYC_2                 0x1F
#define CELL_KEYC_3                 0x20
#define CELL_KEYC_4                 0x21
#define CELL_KEYC_5                 0x22
#define CELL_KEYC_6                 0x23
#define CELL_KEYC_7                 0x24
#define CELL_KEYC_8                 0x25
#define CELL_KEYC_9                 0x26
#define CELL_KEYC_0                 0x27
#define CELL_KEYC_ENTER             0x28
#define CELL_KEYC_ESCAPE            0x29
#define CELL_KEYC_BS                0x2A
#define CELL_KEYC_TAB               0x2B
#define CELL_KEYC_SPACE             0x2C
#define CELL_KEYC_F1                0x3A
#define CELL_KEYC_F2                0x3B
#define CELL_KEYC_F3                0x3C
#define CELL_KEYC_F4                0x3D
#define CELL_KEYC_F5                0x3E
#define CELL_KEYC_F6                0x3F
#define CELL_KEYC_F7                0x40
#define CELL_KEYC_F8                0x41
#define CELL_KEYC_F9                0x42
#define CELL_KEYC_F10               0x43
#define CELL_KEYC_F11               0x44
#define CELL_KEYC_F12               0x45
#define CELL_KEYC_RIGHT_ARROW       0x4F
#define CELL_KEYC_LEFT_ARROW        0x50
#define CELL_KEYC_DOWN_ARROW        0x51
#define CELL_KEYC_UP_ARROW          0x52
#define CELL_KEYC_DELETE            0x4C

/* Connection status */
#define CELL_KB_STATUS_DISCONNECTED     0
#define CELL_KB_STATUS_CONNECTED        1

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/* Keyboard data returned by cellKbGetData */
typedef struct CellKbData {
    u32 led;                            /* LED state flags */
    u32 mkey;                           /* Modifier key flags */
    s32 len;                            /* Number of valid keycodes */
    u16 keycode[CELL_KB_MAX_KEYCODES];  /* Active keycodes */
} CellKbData;

/* Keyboard info returned by cellKbGetInfo */
typedef struct CellKbInfo {
    u32 max_connect;
    u32 now_connect;
    u32 info;                           /* System info flags */
    u32 status[CELL_KB_MAX_KEYBOARDS];  /* Per-keyboard status */
} CellKbInfo;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellKbInit(u32 max_connect);
s32 cellKbEnd(void);
s32 cellKbGetData(u32 port_no, CellKbData* data);
s32 cellKbGetInfo(CellKbInfo* info);
s32 cellKbSetReadMode(u32 port_no, u32 rmode);
s32 cellKbSetCodeType(u32 port_no, u32 codetype);
s32 cellKbClearBuf(u32 port_no);

/* Internal: feed a key event from the host input system */
void cellKb_keyEvent(u32 port, u16 keycode, int pressed);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_KB_H */

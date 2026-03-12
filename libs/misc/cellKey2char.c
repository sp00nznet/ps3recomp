/*
 * ps3recomp - cellKey2char HLE implementation
 *
 * Maps HID keyboard scancodes to ASCII/Unicode characters.
 * Implements US-101 layout; other layouts return the same mapping
 * (sufficient for most recompiled games).
 */

#include "cellKey2char.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * HID usage codes (subset used for key-to-char mapping)
 * -----------------------------------------------------------------------*/
#define HID_KEY_A          0x04
#define HID_KEY_Z          0x1D
#define HID_KEY_1          0x1E
#define HID_KEY_0          0x27
#define HID_KEY_ENTER      0x28
#define HID_KEY_ESCAPE     0x29
#define HID_KEY_BACKSPACE  0x2A
#define HID_KEY_TAB        0x2B
#define HID_KEY_SPACE      0x2C
#define HID_KEY_MINUS      0x2D
#define HID_KEY_EQUAL      0x2E
#define HID_KEY_LBRACKET   0x2F
#define HID_KEY_RBRACKET   0x30
#define HID_KEY_BACKSLASH  0x31
#define HID_KEY_SEMICOLON  0x33
#define HID_KEY_QUOTE      0x34
#define HID_KEY_GRAVE      0x35
#define HID_KEY_COMMA      0x36
#define HID_KEY_PERIOD     0x37
#define HID_KEY_SLASH      0x38

/* Modifier key bits */
#define MKEY_L_CTRL        0x01
#define MKEY_L_SHIFT       0x02
#define MKEY_R_CTRL        0x10
#define MKEY_R_SHIFT       0x20
#define MKEY_SHIFT         (MKEY_L_SHIFT | MKEY_R_SHIFT)

/* LED bits */
#define LED_CAPS_LOCK      0x02

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/
#define MAX_K2C_HANDLES 4

typedef struct {
    int  in_use;
    u32  arrangement;
    s32  mode; /* 0 = normal, 1 = dead-key compose */
} K2CContext;

static K2CContext s_contexts[MAX_K2C_HANDLES];

/* US-101 layout tables */
static const char s_unshifted_symbols[] = {
    /* 0x2D - 0x38: - = [ ] \ # ; ' ` , . / */
    '-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/'
};

static const char s_shifted_symbols[] = {
    '_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?'
};

static const char s_shifted_numbers[] = {
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')'
};

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellKey2CharOpen(u32 arrangement, CellKey2CharHandle* handle)
{
    printf("[cellKey2char] Open(arrangement=%u)\n", arrangement);

    if (!handle)
        return (s32)CELL_K2C_ERROR_INVALID_PARAMETER;

    for (int i = 0; i < MAX_K2C_HANDLES; i++) {
        if (!s_contexts[i].in_use) {
            s_contexts[i].in_use = 1;
            s_contexts[i].arrangement = arrangement;
            s_contexts[i].mode = 0;
            *handle = (u32)i;
            return CELL_OK;
        }
    }

    return (s32)CELL_K2C_ERROR_FATAL;
}

s32 cellKey2CharClose(CellKey2CharHandle handle)
{
    printf("[cellKey2char] Close(handle=%u)\n", handle);

    if (handle >= MAX_K2C_HANDLES || !s_contexts[handle].in_use)
        return (s32)CELL_K2C_ERROR_INVALID_HANDLE;

    s_contexts[handle].in_use = 0;
    return CELL_OK;
}

s32 cellKey2CharKeyCodeToChar(CellKey2CharHandle handle,
                               u32 mkey, u32 led, u16 rawcode,
                               u16* charCode, u32* charNum)
{
    if (handle >= MAX_K2C_HANDLES || !s_contexts[handle].in_use)
        return (s32)CELL_K2C_ERROR_INVALID_HANDLE;

    if (!charCode || !charNum)
        return (s32)CELL_K2C_ERROR_INVALID_PARAMETER;

    int shift = (mkey & MKEY_SHIFT) != 0;
    int caps  = (led & LED_CAPS_LOCK) != 0;

    *charNum = 0;

    /* Letters A-Z (HID 0x04 - 0x1D) */
    if (rawcode >= HID_KEY_A && rawcode <= HID_KEY_Z) {
        char base = 'a' + (char)(rawcode - HID_KEY_A);
        if (shift ^ caps) /* XOR: shift or caps but not both */
            base = (char)(base - 32); /* to uppercase */
        charCode[0] = (u16)base;
        *charNum = 1;
        return CELL_OK;
    }

    /* Numbers 1-9, 0 (HID 0x1E - 0x27) */
    if (rawcode >= HID_KEY_1 && rawcode <= HID_KEY_0) {
        int idx = rawcode - HID_KEY_1;
        if (shift) {
            charCode[0] = (u16)s_shifted_numbers[idx];
        } else {
            charCode[0] = (idx < 9) ? (u16)('1' + idx) : (u16)'0';
        }
        *charNum = 1;
        return CELL_OK;
    }

    /* Special keys */
    switch (rawcode) {
    case HID_KEY_ENTER:
        charCode[0] = '\n';
        *charNum = 1;
        return CELL_OK;
    case HID_KEY_TAB:
        charCode[0] = '\t';
        *charNum = 1;
        return CELL_OK;
    case HID_KEY_SPACE:
        charCode[0] = ' ';
        *charNum = 1;
        return CELL_OK;
    case HID_KEY_BACKSPACE:
        charCode[0] = 0x08;
        *charNum = 1;
        return CELL_OK;
    case HID_KEY_ESCAPE:
        charCode[0] = 0x1B;
        *charNum = 1;
        return CELL_OK;
    }

    /* Symbol keys (0x2D - 0x38) */
    if (rawcode >= HID_KEY_MINUS && rawcode <= HID_KEY_SLASH) {
        int idx = rawcode - HID_KEY_MINUS;
        if (idx < (int)sizeof(s_unshifted_symbols)) {
            charCode[0] = shift ? (u16)s_shifted_symbols[idx]
                                : (u16)s_unshifted_symbols[idx];
            *charNum = 1;
            return CELL_OK;
        }
    }

    /* Unknown key - no character output */
    *charNum = 0;
    return CELL_OK;
}

s32 cellKey2CharSetMode(CellKey2CharHandle handle, s32 mode)
{
    if (handle >= MAX_K2C_HANDLES || !s_contexts[handle].in_use)
        return (s32)CELL_K2C_ERROR_INVALID_HANDLE;

    s_contexts[handle].mode = mode;
    return CELL_OK;
}

s32 cellKey2CharGetMode(CellKey2CharHandle handle, s32* mode)
{
    if (handle >= MAX_K2C_HANDLES || !s_contexts[handle].in_use)
        return (s32)CELL_K2C_ERROR_INVALID_HANDLE;

    if (!mode)
        return (s32)CELL_K2C_ERROR_INVALID_PARAMETER;

    *mode = s_contexts[handle].mode;
    return CELL_OK;
}

/*
 * ps3recomp - cellKb HLE implementation
 *
 * Keyboard input. Tracks key state from host keyboard events and provides
 * data in PS3 CellKbData format.
 *
 * The host input layer (SDL2 event loop, etc.) should call cellKb_keyEvent()
 * whenever a key is pressed or released.
 */

#include "cellKb.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define KB_MAX_TRACKED_KEYS 256

typedef struct {
    int  connected;
    u32  rmode;         /* CELL_KB_RMODE_INPUTCHAR or CELL_KB_RMODE_PACKET */
    u32  codetype;      /* CELL_KB_CODETYPE_RAW or CELL_KB_CODETYPE_ASCII */
    u32  mkey;          /* Current modifier flags */
    u32  led;           /* Current LED flags */
    u8   key_state[KB_MAX_TRACKED_KEYS]; /* 1 = pressed */
    /* Ring buffer for key events (PACKET mode) */
    u16  event_buf[CELL_KB_MAX_KEYCODES];
    s32  event_count;
} KbPortState;

static int         s_kb_initialized = 0;
static u32         s_kb_max_connect = 0;
static KbPortState s_kb_ports[CELL_KB_MAX_KEYBOARDS];

/* ---------------------------------------------------------------------------
 * Host key event injection
 * -----------------------------------------------------------------------*/

void cellKb_keyEvent(u32 port, u16 keycode, int pressed)
{
    if (!s_kb_initialized || port >= CELL_KB_MAX_KEYBOARDS)
        return;

    KbPortState* kb = &s_kb_ports[port];
    if (!kb->connected)
        kb->connected = 1;

    if (keycode < KB_MAX_TRACKED_KEYS)
        kb->key_state[keycode] = pressed ? 1 : 0;

    /* Update modifier flags */
    u32 mflag = 0;
    switch (keycode) {
        case 0xE0: mflag = CELL_KB_MKEY_L_CTRL;  break;
        case 0xE1: mflag = CELL_KB_MKEY_L_SHIFT;  break;
        case 0xE2: mflag = CELL_KB_MKEY_L_ALT;    break;
        case 0xE3: mflag = CELL_KB_MKEY_L_WIN;    break;
        case 0xE4: mflag = CELL_KB_MKEY_R_CTRL;   break;
        case 0xE5: mflag = CELL_KB_MKEY_R_SHIFT;  break;
        case 0xE6: mflag = CELL_KB_MKEY_R_ALT;    break;
        case 0xE7: mflag = CELL_KB_MKEY_R_WIN;    break;
    }
    if (mflag) {
        if (pressed) kb->mkey |= mflag;
        else         kb->mkey &= ~mflag;
    }

    /* In packet mode, buffer the keycode on press */
    if (kb->rmode == CELL_KB_RMODE_PACKET && pressed) {
        if (kb->event_count < CELL_KB_MAX_KEYCODES) {
            kb->event_buf[kb->event_count++] = keycode;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Keycode conversion: RAW (USB HID) to ASCII
 * -----------------------------------------------------------------------*/

static u16 kb_raw_to_ascii(u16 raw, u32 mkey)
{
    int shifted = (mkey & (CELL_KB_MKEY_L_SHIFT | CELL_KB_MKEY_R_SHIFT)) != 0;

    /* Letters A-Z */
    if (raw >= CELL_KEYC_A && raw <= CELL_KEYC_Z) {
        u16 base = (u16)('a' + (raw - CELL_KEYC_A));
        if (shifted) base = (u16)(base - 32);
        return base;
    }
    /* Numbers 1-9,0 */
    if (raw >= CELL_KEYC_1 && raw <= CELL_KEYC_9) {
        if (shifted) {
            const char shifted_nums[] = "!@#$%^&*(";
            return (u16)shifted_nums[raw - CELL_KEYC_1];
        }
        return (u16)('1' + (raw - CELL_KEYC_1));
    }
    if (raw == CELL_KEYC_0) return shifted ? (u16)')' : (u16)'0';
    if (raw == CELL_KEYC_ENTER) return (u16)'\n';
    if (raw == CELL_KEYC_ESCAPE) return (u16)0x1B;
    if (raw == CELL_KEYC_BS) return (u16)'\b';
    if (raw == CELL_KEYC_TAB) return (u16)'\t';
    if (raw == CELL_KEYC_SPACE) return (u16)' ';

    return raw; /* fallback */
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellKbInit(u32 max_connect)
{
    printf("[cellKb] Init(max_connect=%u)\n", max_connect);

    if (s_kb_initialized)
        return CELL_KB_ERROR_ALREADY_INITIALIZED;

    if (max_connect == 0 || max_connect > CELL_KB_MAX_KEYBOARDS)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    s_kb_initialized = 1;
    s_kb_max_connect = max_connect;
    memset(s_kb_ports, 0, sizeof(s_kb_ports));

    /* Assume keyboard 0 is always connected on the host */
    s_kb_ports[0].connected = 1;
    s_kb_ports[0].rmode = CELL_KB_RMODE_INPUTCHAR;
    s_kb_ports[0].codetype = CELL_KB_CODETYPE_ASCII;

    return CELL_OK;
}

s32 cellKbEnd(void)
{
    printf("[cellKb] End()\n");

    if (!s_kb_initialized)
        return CELL_KB_ERROR_UNINITIALIZED;

    s_kb_initialized = 0;
    return CELL_OK;
}

s32 cellKbGetData(u32 port_no, CellKbData* data)
{
    if (!s_kb_initialized)
        return CELL_KB_ERROR_UNINITIALIZED;

    if (port_no >= s_kb_max_connect || !data)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    KbPortState* kb = &s_kb_ports[port_no];

    if (!kb->connected) {
        memset(data, 0, sizeof(CellKbData));
        return CELL_KB_ERROR_NO_DEVICE;
    }

    data->led  = kb->led;
    data->mkey = kb->mkey;

    if (kb->rmode == CELL_KB_RMODE_PACKET) {
        /* Return buffered key events */
        data->len = kb->event_count;
        for (s32 i = 0; i < kb->event_count && i < CELL_KB_MAX_KEYCODES; i++) {
            u16 code = kb->event_buf[i];
            if (kb->codetype == CELL_KB_CODETYPE_ASCII)
                code = kb_raw_to_ascii(code, kb->mkey);
            data->keycode[i] = code;
        }
        /* Clear event buffer after reading */
        kb->event_count = 0;
    } else {
        /* INPUTCHAR mode: report currently held keys */
        s32 count = 0;
        for (int i = 0; i < KB_MAX_TRACKED_KEYS && count < CELL_KB_MAX_KEYCODES; i++) {
            if (kb->key_state[i]) {
                u16 code = (u16)i;
                if (kb->codetype == CELL_KB_CODETYPE_ASCII)
                    code = kb_raw_to_ascii(code, kb->mkey);
                data->keycode[count++] = code;
            }
        }
        data->len = count;
    }

    return CELL_OK;
}

s32 cellKbGetInfo(CellKbInfo* info)
{
    if (!s_kb_initialized)
        return CELL_KB_ERROR_UNINITIALIZED;

    if (!info)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    memset(info, 0, sizeof(CellKbInfo));
    info->max_connect = s_kb_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_kb_max_connect; i++) {
        if (s_kb_ports[i].connected) {
            info->status[i] = CELL_KB_STATUS_CONNECTED;
            connected++;
        }
    }
    info->now_connect = connected;

    return CELL_OK;
}

s32 cellKbSetReadMode(u32 port_no, u32 rmode)
{
    printf("[cellKb] SetReadMode(port=%u, rmode=%u)\n", port_no, rmode);

    if (!s_kb_initialized)
        return CELL_KB_ERROR_UNINITIALIZED;

    if (port_no >= CELL_KB_MAX_KEYBOARDS)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    if (rmode != CELL_KB_RMODE_INPUTCHAR && rmode != CELL_KB_RMODE_PACKET)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    s_kb_ports[port_no].rmode = rmode;
    return CELL_OK;
}

s32 cellKbSetCodeType(u32 port_no, u32 codetype)
{
    printf("[cellKb] SetCodeType(port=%u, codetype=%u)\n", port_no, codetype);

    if (!s_kb_initialized)
        return CELL_KB_ERROR_UNINITIALIZED;

    if (port_no >= CELL_KB_MAX_KEYBOARDS)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    if (codetype != CELL_KB_CODETYPE_RAW && codetype != CELL_KB_CODETYPE_ASCII)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    s_kb_ports[port_no].codetype = codetype;
    return CELL_OK;
}

s32 cellKbClearBuf(u32 port_no)
{
    if (!s_kb_initialized)
        return CELL_KB_ERROR_UNINITIALIZED;

    if (port_no >= CELL_KB_MAX_KEYBOARDS)
        return CELL_KB_ERROR_INVALID_PARAMETER;

    memset(s_kb_ports[port_no].key_state, 0, sizeof(s_kb_ports[port_no].key_state));
    s_kb_ports[port_no].event_count = 0;
    return CELL_OK;
}

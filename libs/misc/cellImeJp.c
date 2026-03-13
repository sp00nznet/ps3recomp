/*
 * ps3recomp - cellImeJp HLE implementation
 *
 * Japanese Input Method Editor.
 * Stub — accepts input characters but no kana-to-kanji conversion.
 * GetConvertedString returns the raw input as-is (passthrough).
 */

#include "cellImeJp.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

#define IMEJP_MAX_HANDLES 4

typedef struct {
    int  in_use;
    u32  inputMode;
    u16  input[CELL_IMEJP_MAX_INPUT_LENGTH];
    u32  inputLen;
} ImeJpSlot;

static ImeJpSlot s_slots[IMEJP_MAX_HANDLES];

static int imejp_alloc(void)
{
    for (int i = 0; i < IMEJP_MAX_HANDLES; i++) {
        if (!s_slots[i].in_use) return i;
    }
    return -1;
}

/* API */

s32 cellImeJpOpen(const CellImeJpConfig* config, CellImeJpHandle* handle)
{
    printf("[cellImeJp] Open()\n");
    if (!handle) return (s32)CELL_IMEJP_ERROR_INVALID_ARGUMENT;

    int slot = imejp_alloc();
    if (slot < 0) return (s32)CELL_IMEJP_ERROR_OUT_OF_MEMORY;

    s_slots[slot].in_use = 1;
    s_slots[slot].inputMode = config ? config->inputMode : CELL_IMEJP_INPUT_MODE_HIRAGANA;
    s_slots[slot].inputLen = 0;
    memset(s_slots[slot].input, 0, sizeof(s_slots[slot].input));

    *handle = (CellImeJpHandle)slot;
    return CELL_OK;
}

s32 cellImeJpClose(CellImeJpHandle handle)
{
    printf("[cellImeJp] Close(%u)\n", handle);
    if (handle >= IMEJP_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_IMEJP_ERROR_INVALID_ARGUMENT;

    s_slots[handle].in_use = 0;
    return CELL_OK;
}

s32 cellImeJpReset(CellImeJpHandle handle)
{
    if (handle >= IMEJP_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_IMEJP_ERROR_INVALID_ARGUMENT;

    s_slots[handle].inputLen = 0;
    memset(s_slots[handle].input, 0, sizeof(s_slots[handle].input));
    return CELL_OK;
}

s32 cellImeJpSetInputMode(CellImeJpHandle handle, u32 mode)
{
    if (handle >= IMEJP_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_IMEJP_ERROR_INVALID_ARGUMENT;

    s_slots[handle].inputMode = mode;
    return CELL_OK;
}

s32 cellImeJpGetInputMode(CellImeJpHandle handle, u32* mode)
{
    if (handle >= IMEJP_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_IMEJP_ERROR_INVALID_ARGUMENT;
    if (!mode) return (s32)CELL_IMEJP_ERROR_INVALID_ARGUMENT;

    *mode = s_slots[handle].inputMode;
    return CELL_OK;
}

s32 cellImeJpAddChar(CellImeJpHandle handle, u16 ch)
{
    if (handle >= IMEJP_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_IMEJP_ERROR_INVALID_ARGUMENT;

    ImeJpSlot* s = &s_slots[handle];
    if (s->inputLen >= CELL_IMEJP_MAX_INPUT_LENGTH - 1)
        return (s32)CELL_IMEJP_ERROR_OUT_OF_MEMORY;

    s->input[s->inputLen++] = ch;
    return CELL_OK;
}

s32 cellImeJpBackspace(CellImeJpHandle handle)
{
    if (handle >= IMEJP_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_IMEJP_ERROR_INVALID_ARGUMENT;

    ImeJpSlot* s = &s_slots[handle];
    if (s->inputLen > 0) {
        s->inputLen--;
        s->input[s->inputLen] = 0;
    }
    return CELL_OK;
}

s32 cellImeJpConfirm(CellImeJpHandle handle)
{
    if (handle >= IMEJP_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_IMEJP_ERROR_INVALID_ARGUMENT;

    /* No-op: passthrough mode, input is already the "converted" result */
    return CELL_OK;
}

s32 cellImeJpGetInputString(CellImeJpHandle handle, u16* str, u32* len)
{
    if (handle >= IMEJP_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_IMEJP_ERROR_INVALID_ARGUMENT;
    if (!str || !len) return (s32)CELL_IMEJP_ERROR_INVALID_ARGUMENT;

    ImeJpSlot* s = &s_slots[handle];
    u32 copyLen = s->inputLen;
    if (copyLen > *len) copyLen = *len;

    memcpy(str, s->input, copyLen * sizeof(u16));
    *len = copyLen;
    return CELL_OK;
}

s32 cellImeJpGetConvertedString(CellImeJpHandle handle, u16* str, u32* len)
{
    /* Passthrough: converted string = raw input (no kanji conversion) */
    return cellImeJpGetInputString(handle, str, len);
}

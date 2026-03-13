/*
 * ps3recomp - cellImeJp HLE
 *
 * Japanese Input Method Editor. Handles kana-to-kanji conversion.
 * Stub — init/end lifecycle, input accepted but conversion is passthrough.
 */

#ifndef PS3RECOMP_CELL_IMEJP_H
#define PS3RECOMP_CELL_IMEJP_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_IMEJP_ERROR_NOT_INITIALIZED     0x80040601
#define CELL_IMEJP_ERROR_ALREADY_INITIALIZED 0x80040602
#define CELL_IMEJP_ERROR_INVALID_ARGUMENT    0x80040603
#define CELL_IMEJP_ERROR_OUT_OF_MEMORY       0x80040604
#define CELL_IMEJP_ERROR_NOT_SUPPORTED       0x80040605
#define CELL_IMEJP_ERROR_CONTEXT_DESTROYED   0x80040606

/* Input mode */
#define CELL_IMEJP_INPUT_MODE_HIRAGANA      0
#define CELL_IMEJP_INPUT_MODE_KATAKANA      1
#define CELL_IMEJP_INPUT_MODE_HALF_KATAKANA 2
#define CELL_IMEJP_INPUT_MODE_ALPHA         3

/* Max text lengths */
#define CELL_IMEJP_MAX_INPUT_LENGTH   256
#define CELL_IMEJP_MAX_RESULT_LENGTH  512

/* Handle */
typedef u32 CellImeJpHandle;

/* Config */
typedef struct CellImeJpConfig {
    void* dictionary;
    u32 dictionarySize;
    u32 inputMode;
    u32 flags;
    u32 reserved[4];
} CellImeJpConfig;

/* Functions */
s32 cellImeJpOpen(const CellImeJpConfig* config, CellImeJpHandle* handle);
s32 cellImeJpClose(CellImeJpHandle handle);
s32 cellImeJpReset(CellImeJpHandle handle);

s32 cellImeJpSetInputMode(CellImeJpHandle handle, u32 mode);
s32 cellImeJpGetInputMode(CellImeJpHandle handle, u32* mode);

s32 cellImeJpAddChar(CellImeJpHandle handle, u16 ch);
s32 cellImeJpBackspace(CellImeJpHandle handle);
s32 cellImeJpConfirm(CellImeJpHandle handle);

s32 cellImeJpGetInputString(CellImeJpHandle handle, u16* str, u32* len);
s32 cellImeJpGetConvertedString(CellImeJpHandle handle, u16* str, u32* len);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_IMEJP_H */

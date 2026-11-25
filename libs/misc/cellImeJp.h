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

/* Error codes (sysutil/sysutil_imejp.h; base 0x8002BF00 verified against Sony
 * firmware 4.93 libsysutil_imejp.sprx: cellImeJpReset(0) -> 0x8002BF11 CONTEXT,
 * cellImeJpOpen3(0,...) -> 0x8002BF41 PARAM) */
#define CELL_IMEJP_ERROR_NOT_INITIALIZED     (CELL_ERROR_BASE_SYSUTIL_IMEJP | 0x01) /* 0x8002BF01 */
#define CELL_IMEJP_ERROR_ALREADY_INITIALIZED (CELL_ERROR_BASE_SYSUTIL_IMEJP | 0x02) /* 0x8002BF02 */
#define CELL_IMEJP_ERROR_CONTEXT             (CELL_ERROR_BASE_SYSUTIL_IMEJP | 0x11) /* 0x8002BF11 */
#define CELL_IMEJP_ERROR_CONTEXT_DESTROYED   CELL_IMEJP_ERROR_CONTEXT
#define CELL_IMEJP_ERROR_PARAM               (CELL_ERROR_BASE_SYSUTIL_IMEJP | 0x41) /* 0x8002BF41 */
#define CELL_IMEJP_ERROR_INVALID_ARGUMENT    CELL_IMEJP_ERROR_PARAM
#define CELL_IMEJP_ERROR_OUT_OF_MEMORY       (CELL_ERROR_BASE_SYSUTIL_IMEJP | 0x42) /* 0x8002BF42 */
#define CELL_IMEJP_ERROR_NOT_SUPPORTED       (CELL_ERROR_BASE_SYSUTIL_IMEJP | 0x43) /* 0x8002BF43 */

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

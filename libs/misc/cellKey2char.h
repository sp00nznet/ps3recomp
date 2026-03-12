/*
 * ps3recomp - cellKey2char HLE
 *
 * Keyboard scancode to character conversion. Maps PS3 keyboard HID
 * scancodes to Unicode characters based on keyboard arrangement.
 */

#ifndef PS3RECOMP_CELL_KEY2CHAR_H
#define PS3RECOMP_CELL_KEY2CHAR_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_K2C_ERROR_FATAL               0x80121601
#define CELL_K2C_ERROR_INVALID_HANDLE      0x80121602
#define CELL_K2C_ERROR_INVALID_PARAMETER   0x80121603
#define CELL_K2C_ERROR_ALREADY_INITIALIZED 0x80121604
#define CELL_K2C_ERROR_UNINITIALIZED       0x80121605

/* ---------------------------------------------------------------------------
 * Keyboard arrangements
 * -----------------------------------------------------------------------*/
#define CELL_KB_MAPPING_101                0
#define CELL_KB_MAPPING_106                1
#define CELL_KB_MAPPING_106_KANA           2
#define CELL_KB_MAPPING_GERMAN_GERMANY     3
#define CELL_KB_MAPPING_SPANISH_SPAIN      4
#define CELL_KB_MAPPING_FRENCH_FRANCE      5
#define CELL_KB_MAPPING_ITALIAN_ITALY      6
#define CELL_KB_MAPPING_PORTUGUESE_PORTUGAL 7
#define CELL_KB_MAPPING_RUSSIAN_RUSSIA     8
#define CELL_KB_MAPPING_ENGLISH_UK         9
#define CELL_KB_MAPPING_KOREAN_KOREA       10
#define CELL_KB_MAPPING_NORWEGIAN_NORWAY   11
#define CELL_KB_MAPPING_FINNISH_FINLAND    12
#define CELL_KB_MAPPING_DANISH_DENMARK     13
#define CELL_KB_MAPPING_SWEDISH_SWEDEN     14
#define CELL_KB_MAPPING_CHINESE_TRADITIONAL 15
#define CELL_KB_MAPPING_CHINESE_SIMPLIFIED 16
#define CELL_KB_MAPPING_SWISS_FRENCH       17
#define CELL_KB_MAPPING_SWISS_GERMAN       18
#define CELL_KB_MAPPING_CANADIAN_FRENCH    19
#define CELL_KB_MAPPING_BELGIAN_BELGIUM    20
#define CELL_KB_MAPPING_POLISH_POLAND      21
#define CELL_KB_MAPPING_BRAZILIAN          22
#define CELL_KB_MAPPING_TURKISH_TURKEY     23

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef u32 CellKey2CharHandle;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellKey2CharOpen(u32 arrangement, CellKey2CharHandle* handle);
s32 cellKey2CharClose(CellKey2CharHandle handle);

/* Convert a keyboard scancode to Unicode character(s) */
s32 cellKey2CharKeyCodeToChar(CellKey2CharHandle handle,
                               u32 mkey,        /* modifier keys */
                               u32 led,         /* LED state */
                               u16 rawcode,     /* HID usage code */
                               u16* charCode,   /* output char */
                               u32* charNum);   /* number of output chars */

/* Set/get dead key composition mode */
s32 cellKey2CharSetMode(CellKey2CharHandle handle, s32 mode);
s32 cellKey2CharGetMode(CellKey2CharHandle handle, s32* mode);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_KEY2CHAR_H */

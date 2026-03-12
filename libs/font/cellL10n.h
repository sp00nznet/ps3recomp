/*
 * ps3recomp - cellL10n HLE
 *
 * Character encoding conversion: UTF-8, UTF-16, UTF-32, Shift-JIS, EUC-JP,
 * ISO-8859-1, and other encodings used by PS3 titles.
 */

#ifndef PS3RECOMP_CELL_L10N_H
#define PS3RECOMP_CELL_L10N_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_L10N_ERROR_NOT_SUPPORTED      0x80610140
#define CELL_L10N_ERROR_ILLEGAL_SEQUENCE   0x80610141
#define CELL_L10N_ERROR_INSUFFICIENT       0x80610142
#define CELL_L10N_ERROR_INVALID_PARAM      0x80610143

/* ---------------------------------------------------------------------------
 * Encoding IDs
 * -----------------------------------------------------------------------*/
#define L10N_UTF8               0
#define L10N_UTF16              1
#define L10N_UTF32              2
#define L10N_UCS2               3
#define L10N_SHIFT_JIS          4   /* CP932 */
#define L10N_EUC_JP             5
#define L10N_ISO_8859_1         6
#define L10N_ISO_8859_2         7
#define L10N_ISO_8859_3         8
#define L10N_ISO_8859_4         9
#define L10N_ISO_8859_5         10
#define L10N_ISO_8859_6         11
#define L10N_ISO_8859_7         12
#define L10N_ISO_8859_8         13
#define L10N_ISO_8859_9         14
#define L10N_ISO_8859_10        15
#define L10N_ISO_8859_13        16
#define L10N_ISO_8859_14        17
#define L10N_ISO_8859_15        18
#define L10N_GB18030            19
#define L10N_BIG5               20
#define L10N_EUC_KR             21
#define L10N_ASCII              22
#define L10N_CODEPAGE_MAX       23

/* ---------------------------------------------------------------------------
 * Unicode BOM
 * -----------------------------------------------------------------------*/
#define L10N_BOM_NONE           0
#define L10N_BOM_PREPEND        1

/* ---------------------------------------------------------------------------
 * Single-character conversion
 * -----------------------------------------------------------------------*/

/* UTF-8 <-> UTF-16 */
s32 UTF8toUTF16(const u8* utf8, u32 utf8_len,
                u16* utf16, u32* utf16_len);
s32 UTF16toUTF8(const u16* utf16, u32 utf16_len,
                u8* utf8, u32* utf8_len);

/* UTF-8 <-> UTF-32 */
s32 UTF8toUTF32(const u8* utf8, u32 utf8_len,
                u32* utf32, u32* utf32_len);
s32 UTF32toUTF8(const u32* utf32, u32 utf32_len,
                u8* utf8, u32* utf8_len);

/* UTF-16 <-> UTF-32 */
s32 UTF16toUTF32(const u16* utf16, u32 utf16_len,
                 u32* utf32, u32* utf32_len);
s32 UTF32toUTF16(const u32* utf32, u32 utf32_len,
                 u16* utf16, u32* utf16_len);

/* UCS-2 (BMP only) */
s32 UTF8toUCS2(const u8* utf8, u32 utf8_len,
               u16* ucs2, u32* ucs2_len);
s32 UCS2toUTF8(const u16* ucs2, u32 ucs2_len,
               u8* utf8, u32* utf8_len);

/* ---------------------------------------------------------------------------
 * String conversion (null-terminated)
 * -----------------------------------------------------------------------*/
s32 UTF8stoUTF16s(const u8* utf8, u32* utf8_len,
                  u16* utf16, u32* utf16_len);
s32 UTF16stoUTF8s(const u16* utf16, u32* utf16_len,
                  u8* utf8, u32* utf8_len);

/* ---------------------------------------------------------------------------
 * Utility
 * -----------------------------------------------------------------------*/

/* Count bytes needed for conversion */
s32 UTF8toUTF16Size(const u8* utf8, u32 utf8_len, u32* out_size);
s32 UTF16toUTF8Size(const u16* utf16, u32 utf16_len, u32* out_size);

/* Encoding queries */
s32 l10n_get_converter(u32 src_encoding, u32 dst_encoding);
s32 l10n_convert(u32 src_encoding, const void* src, u32 src_len,
                 u32 dst_encoding, void* dst, u32 dst_max, u32* dst_len);
s32 l10n_convert_str(s32 src_code, const void* src, u32* src_len,
                     s32 dst_code, void* dst, u32* dst_len);

/* Single codepoint length in UTF-8 */
s32 UTF8Len(const u8* utf8);
/* Single codepoint length in UTF-16 */
s32 UTF16Len(const u16* utf16);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_L10N_H */

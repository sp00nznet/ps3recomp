/*
 * ps3recomp - cellL10n HLE implementation
 *
 * Character encoding conversion using hand-rolled UTF codecs.
 * Covers the common UTF-8/16/32 conversions that PS3 games rely on.
 * Exotic encodings (Shift-JIS, EUC-JP, GB18030) return NOT_SUPPORTED
 * unless a future CJK table is added.
 */

#include "cellL10n.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal UTF helpers
 * -----------------------------------------------------------------------*/

/* Decode one UTF-8 codepoint. Returns bytes consumed, 0 on error. */
static int decode_utf8(const u8* s, u32 len, u32* cp)
{
    if (len == 0) return 0;

    u8 b = s[0];
    if (b < 0x80) {
        *cp = b;
        return 1;
    } else if ((b & 0xE0) == 0xC0) {
        if (len < 2 || (s[1] & 0xC0) != 0x80) return 0;
        *cp = ((u32)(b & 0x1F) << 6) | (s[1] & 0x3F);
        if (*cp < 0x80) return 0; /* overlong */
        return 2;
    } else if ((b & 0xF0) == 0xE0) {
        if (len < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        *cp = ((u32)(b & 0x0F) << 12) | ((u32)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (*cp < 0x800) return 0;
        if (*cp >= 0xD800 && *cp <= 0xDFFF) return 0; /* surrogate */
        return 3;
    } else if ((b & 0xF8) == 0xF0) {
        if (len < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80)
            return 0;
        *cp = ((u32)(b & 0x07) << 18) | ((u32)(s[1] & 0x3F) << 12)
            | ((u32)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        if (*cp < 0x10000 || *cp > 0x10FFFF) return 0;
        return 4;
    }
    return 0;
}

/* Encode one codepoint to UTF-8. Returns bytes written. */
static int encode_utf8(u32 cp, u8* out, u32 max)
{
    if (cp < 0x80) {
        if (max < 1) return 0;
        out[0] = (u8)cp;
        return 1;
    } else if (cp < 0x800) {
        if (max < 2) return 0;
        out[0] = 0xC0 | (u8)(cp >> 6);
        out[1] = 0x80 | (u8)(cp & 0x3F);
        return 2;
    } else if (cp < 0x10000) {
        if (max < 3) return 0;
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        out[0] = 0xE0 | (u8)(cp >> 12);
        out[1] = 0x80 | (u8)((cp >> 6) & 0x3F);
        out[2] = 0x80 | (u8)(cp & 0x3F);
        return 3;
    } else if (cp <= 0x10FFFF) {
        if (max < 4) return 0;
        out[0] = 0xF0 | (u8)(cp >> 18);
        out[1] = 0x80 | (u8)((cp >> 12) & 0x3F);
        out[2] = 0x80 | (u8)((cp >> 6) & 0x3F);
        out[3] = 0x80 | (u8)(cp & 0x3F);
        return 4;
    }
    return 0;
}

/* Decode one UTF-16 codepoint (handles surrogates). Returns u16s consumed. */
static int decode_utf16(const u16* s, u32 len, u32* cp)
{
    if (len == 0) return 0;

    u16 w = s[0];
    if (w >= 0xD800 && w <= 0xDBFF) {
        /* High surrogate */
        if (len < 2) return 0;
        u16 lo = s[1];
        if (lo < 0xDC00 || lo > 0xDFFF) return 0;
        *cp = 0x10000 + ((u32)(w - 0xD800) << 10) + (lo - 0xDC00);
        return 2;
    } else if (w >= 0xDC00 && w <= 0xDFFF) {
        return 0; /* lone low surrogate */
    }
    *cp = w;
    return 1;
}

/* Encode one codepoint to UTF-16. Returns u16s written. */
static int encode_utf16(u32 cp, u16* out, u32 max)
{
    if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        if (max < 1) return 0;
        out[0] = (u16)cp;
        return 1;
    } else if (cp <= 0x10FFFF) {
        if (max < 2) return 0;
        cp -= 0x10000;
        out[0] = 0xD800 + (u16)(cp >> 10);
        out[1] = 0xDC00 + (u16)(cp & 0x3FF);
        return 2;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * UTF-8 <-> UTF-16
 * -----------------------------------------------------------------------*/

s32 UTF8toUTF16(const u8* utf8, u32 utf8_len,
                u16* utf16, u32* utf16_len)
{
    if (!utf8 || !utf16 || !utf16_len)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 out_max = *utf16_len;
    u32 out_pos = 0;
    u32 in_pos = 0;

    while (in_pos < utf8_len) {
        u32 cp;
        int consumed = decode_utf8(utf8 + in_pos, utf8_len - in_pos, &cp);
        if (consumed == 0)
            return (s32)CELL_L10N_ERROR_ILLEGAL_SEQUENCE;

        int written = encode_utf16(cp, utf16 + out_pos, out_max - out_pos);
        if (written == 0)
            return (s32)CELL_L10N_ERROR_INSUFFICIENT;

        in_pos += consumed;
        out_pos += written;
    }

    *utf16_len = out_pos;
    return CELL_OK;
}

s32 UTF16toUTF8(const u16* utf16, u32 utf16_len,
                u8* utf8, u32* utf8_len)
{
    if (!utf16 || !utf8 || !utf8_len)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 out_max = *utf8_len;
    u32 out_pos = 0;
    u32 in_pos = 0;

    while (in_pos < utf16_len) {
        u32 cp;
        int consumed = decode_utf16(utf16 + in_pos, utf16_len - in_pos, &cp);
        if (consumed == 0)
            return (s32)CELL_L10N_ERROR_ILLEGAL_SEQUENCE;

        int written = encode_utf8(cp, utf8 + out_pos, out_max - out_pos);
        if (written == 0)
            return (s32)CELL_L10N_ERROR_INSUFFICIENT;

        in_pos += consumed;
        out_pos += written;
    }

    *utf8_len = out_pos;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * UTF-8 <-> UTF-32
 * -----------------------------------------------------------------------*/

s32 UTF8toUTF32(const u8* utf8, u32 utf8_len,
                u32* utf32, u32* utf32_len)
{
    if (!utf8 || !utf32 || !utf32_len)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 out_max = *utf32_len;
    u32 out_pos = 0;
    u32 in_pos = 0;

    while (in_pos < utf8_len) {
        u32 cp;
        int consumed = decode_utf8(utf8 + in_pos, utf8_len - in_pos, &cp);
        if (consumed == 0)
            return (s32)CELL_L10N_ERROR_ILLEGAL_SEQUENCE;

        if (out_pos >= out_max)
            return (s32)CELL_L10N_ERROR_INSUFFICIENT;

        utf32[out_pos++] = cp;
        in_pos += consumed;
    }

    *utf32_len = out_pos;
    return CELL_OK;
}

s32 UTF32toUTF8(const u32* utf32, u32 utf32_len,
                u8* utf8, u32* utf8_len)
{
    if (!utf32 || !utf8 || !utf8_len)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 out_max = *utf8_len;
    u32 out_pos = 0;

    for (u32 i = 0; i < utf32_len; i++) {
        int written = encode_utf8(utf32[i], utf8 + out_pos, out_max - out_pos);
        if (written == 0)
            return (s32)CELL_L10N_ERROR_INSUFFICIENT;
        out_pos += written;
    }

    *utf8_len = out_pos;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * UTF-16 <-> UTF-32
 * -----------------------------------------------------------------------*/

s32 UTF16toUTF32(const u16* utf16, u32 utf16_len,
                 u32* utf32, u32* utf32_len)
{
    if (!utf16 || !utf32 || !utf32_len)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 out_max = *utf32_len;
    u32 out_pos = 0;
    u32 in_pos = 0;

    while (in_pos < utf16_len) {
        u32 cp;
        int consumed = decode_utf16(utf16 + in_pos, utf16_len - in_pos, &cp);
        if (consumed == 0)
            return (s32)CELL_L10N_ERROR_ILLEGAL_SEQUENCE;

        if (out_pos >= out_max)
            return (s32)CELL_L10N_ERROR_INSUFFICIENT;

        utf32[out_pos++] = cp;
        in_pos += consumed;
    }

    *utf32_len = out_pos;
    return CELL_OK;
}

s32 UTF32toUTF16(const u32* utf32, u32 utf32_len,
                 u16* utf16, u32* utf16_len)
{
    if (!utf32 || !utf16 || !utf16_len)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 out_max = *utf16_len;
    u32 out_pos = 0;

    for (u32 i = 0; i < utf32_len; i++) {
        int written = encode_utf16(utf32[i], utf16 + out_pos, out_max - out_pos);
        if (written == 0)
            return (s32)CELL_L10N_ERROR_INSUFFICIENT;
        out_pos += written;
    }

    *utf16_len = out_pos;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * UCS-2 (BMP-only subset of UTF-16, no surrogates)
 * -----------------------------------------------------------------------*/

s32 UTF8toUCS2(const u8* utf8, u32 utf8_len,
               u16* ucs2, u32* ucs2_len)
{
    if (!utf8 || !ucs2 || !ucs2_len)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 out_max = *ucs2_len;
    u32 out_pos = 0;
    u32 in_pos = 0;

    while (in_pos < utf8_len) {
        u32 cp;
        int consumed = decode_utf8(utf8 + in_pos, utf8_len - in_pos, &cp);
        if (consumed == 0)
            return (s32)CELL_L10N_ERROR_ILLEGAL_SEQUENCE;

        if (cp > 0xFFFF)
            return (s32)CELL_L10N_ERROR_ILLEGAL_SEQUENCE;

        if (out_pos >= out_max)
            return (s32)CELL_L10N_ERROR_INSUFFICIENT;

        ucs2[out_pos++] = (u16)cp;
        in_pos += consumed;
    }

    *ucs2_len = out_pos;
    return CELL_OK;
}

s32 UCS2toUTF8(const u16* ucs2, u32 ucs2_len,
               u8* utf8, u32* utf8_len)
{
    /* UCS-2 is just UTF-16 without surrogates */
    return UTF16toUTF8(ucs2, ucs2_len, utf8, utf8_len);
}

/* ---------------------------------------------------------------------------
 * String conversion (null-terminated)
 * -----------------------------------------------------------------------*/

s32 UTF8stoUTF16s(const u8* utf8, u32* utf8_len,
                  u16* utf16, u32* utf16_len)
{
    if (!utf8 || !utf8_len || !utf16 || !utf16_len)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    /* Find actual string length (up to *utf8_len) */
    u32 src_len = 0;
    while (src_len < *utf8_len && utf8[src_len] != 0)
        src_len++;

    u32 dst_max = *utf16_len;
    s32 rc = UTF8toUTF16(utf8, src_len, utf16, &dst_max);
    if (rc != CELL_OK) return rc;

    /* Null-terminate if space available */
    if (dst_max < *utf16_len)
        utf16[dst_max] = 0;

    *utf8_len = src_len;
    *utf16_len = dst_max;
    return CELL_OK;
}

s32 UTF16stoUTF8s(const u16* utf16, u32* utf16_len,
                  u8* utf8, u32* utf8_len)
{
    if (!utf16 || !utf16_len || !utf8 || !utf8_len)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 src_len = 0;
    while (src_len < *utf16_len && utf16[src_len] != 0)
        src_len++;

    u32 dst_max = *utf8_len;
    s32 rc = UTF16toUTF8(utf16, src_len, utf8, &dst_max);
    if (rc != CELL_OK) return rc;

    if (dst_max < *utf8_len)
        utf8[dst_max] = 0;

    *utf16_len = src_len;
    *utf8_len = dst_max;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Size queries
 * -----------------------------------------------------------------------*/

s32 UTF8toUTF16Size(const u8* utf8, u32 utf8_len, u32* out_size)
{
    if (!utf8 || !out_size)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 count = 0;
    u32 pos = 0;

    while (pos < utf8_len) {
        u32 cp;
        int consumed = decode_utf8(utf8 + pos, utf8_len - pos, &cp);
        if (consumed == 0)
            return (s32)CELL_L10N_ERROR_ILLEGAL_SEQUENCE;
        pos += consumed;
        count += (cp >= 0x10000) ? 2 : 1;
    }

    *out_size = count;
    return CELL_OK;
}

s32 UTF16toUTF8Size(const u16* utf16, u32 utf16_len, u32* out_size)
{
    if (!utf16 || !out_size)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 count = 0;
    u32 pos = 0;

    while (pos < utf16_len) {
        u32 cp;
        int consumed = decode_utf16(utf16 + pos, utf16_len - pos, &cp);
        if (consumed == 0)
            return (s32)CELL_L10N_ERROR_ILLEGAL_SEQUENCE;
        pos += consumed;

        if (cp < 0x80) count += 1;
        else if (cp < 0x800) count += 2;
        else if (cp < 0x10000) count += 3;
        else count += 4;
    }

    *out_size = count;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Codepoint length queries
 * -----------------------------------------------------------------------*/

s32 UTF8Len(const u8* utf8)
{
    if (!utf8) return 0;

    u8 b = utf8[0];
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 0; /* invalid */
}

s32 UTF16Len(const u16* utf16)
{
    if (!utf16) return 0;

    u16 w = utf16[0];
    if (w >= 0xD800 && w <= 0xDBFF) return 2; /* surrogate pair */
    return 1;
}

/* ---------------------------------------------------------------------------
 * Generic converter interface
 * -----------------------------------------------------------------------*/

s32 l10n_get_converter(u32 src_encoding, u32 dst_encoding)
{
    /* We support UTF-family conversions natively */
    if (src_encoding <= L10N_UCS2 && dst_encoding <= L10N_UCS2)
        return CELL_OK;

    /* ASCII <-> UTF-8 is trivial */
    if ((src_encoding == L10N_ASCII && dst_encoding == L10N_UTF8) ||
        (src_encoding == L10N_UTF8 && dst_encoding == L10N_ASCII))
        return CELL_OK;

    /* ISO-8859-1 <-> UTF conversions */
    if (src_encoding == L10N_ISO_8859_1 && dst_encoding <= L10N_UCS2)
        return CELL_OK;
    if (src_encoding <= L10N_UCS2 && dst_encoding == L10N_ISO_8859_1)
        return CELL_OK;

    printf("[cellL10n] get_converter: unsupported %u -> %u\n",
           src_encoding, dst_encoding);
    return (s32)CELL_L10N_ERROR_NOT_SUPPORTED;
}

s32 l10n_convert(u32 src_encoding, const void* src, u32 src_len,
                 u32 dst_encoding, void* dst, u32 dst_max, u32* dst_len)
{
    if (!src || !dst || !dst_len)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    /* UTF-8 -> UTF-16 */
    if (src_encoding == L10N_UTF8 && dst_encoding == L10N_UTF16) {
        u32 out_len = dst_max / sizeof(u16);
        s32 rc = UTF8toUTF16((const u8*)src, src_len,
                             (u16*)dst, &out_len);
        *dst_len = out_len * sizeof(u16);
        return rc;
    }

    /* UTF-16 -> UTF-8 */
    if (src_encoding == L10N_UTF16 && dst_encoding == L10N_UTF8) {
        u32 out_len = dst_max;
        s32 rc = UTF16toUTF8((const u16*)src, src_len / sizeof(u16),
                             (u8*)dst, &out_len);
        *dst_len = out_len;
        return rc;
    }

    /* Same encoding - just copy */
    if (src_encoding == dst_encoding) {
        u32 copy_len = src_len < dst_max ? src_len : dst_max;
        memcpy(dst, src, copy_len);
        *dst_len = copy_len;
        return CELL_OK;
    }

    /* ASCII -> UTF-8 (identity for 7-bit) */
    if (src_encoding == L10N_ASCII && dst_encoding == L10N_UTF8) {
        u32 copy_len = src_len < dst_max ? src_len : dst_max;
        memcpy(dst, src, copy_len);
        *dst_len = copy_len;
        return CELL_OK;
    }

    /* ISO-8859-1 -> UTF-8 */
    if (src_encoding == L10N_ISO_8859_1 && dst_encoding == L10N_UTF8) {
        const u8* s = (const u8*)src;
        u8* d = (u8*)dst;
        u32 out_pos = 0;
        for (u32 i = 0; i < src_len; i++) {
            int w = encode_utf8(s[i], d + out_pos, dst_max - out_pos);
            if (w == 0)
                return (s32)CELL_L10N_ERROR_INSUFFICIENT;
            out_pos += w;
        }
        *dst_len = out_pos;
        return CELL_OK;
    }

    /* ISO-8859-1 -> UTF-16 */
    if (src_encoding == L10N_ISO_8859_1 && dst_encoding == L10N_UTF16) {
        const u8* s = (const u8*)src;
        u16* d = (u16*)dst;
        u32 max_units = dst_max / sizeof(u16);
        u32 count = src_len < max_units ? src_len : max_units;
        for (u32 i = 0; i < count; i++)
            d[i] = s[i]; /* ISO-8859-1 codepoints = Unicode codepoints */
        *dst_len = count * sizeof(u16);
        return CELL_OK;
    }

    printf("[cellL10n] convert: unsupported %u -> %u\n",
           src_encoding, dst_encoding);
    return (s32)CELL_L10N_ERROR_NOT_SUPPORTED;
}

s32 l10n_convert_str(s32 src_code, const void* src, u32* src_len,
                     s32 dst_code, void* dst, u32* dst_len)
{
    if (!src || !src_len || !dst || !dst_len)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 out_len = *dst_len;
    s32 rc = l10n_convert((u32)src_code, src, *src_len,
                          (u32)dst_code, dst, out_len, dst_len);
    return rc;
}

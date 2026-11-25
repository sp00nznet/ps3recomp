/*
 * ps3recomp - cellL10n HLE implementation
 *
 * Character encoding conversion against big-endian PS3 guest memory
 * (libl10n.sprx): UTF-8, UTF-16, UTF-32, UCS-2, Shift-JIS, ISO-8859-1, ASCII.
 *
 * All pointer parameters arrive as 32-bit guest effective addresses from the
 * HLE dispatcher; multi-byte code units and length out-params use big-endian
 * guest byte order (vm_read16/32, vm_write16/32).
 */

#include "cellL10n.h"
#include "ps3emu/endian.h"
#include "../../runtime/ppu/ppu_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal UTF helpers (host buffers / guest EAs)
 * -----------------------------------------------------------------------*/

/* Decode one UTF-8 codepoint from host byte pointer. Returns bytes consumed, 0 on error. */
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

/* Encode one codepoint to UTF-8. Returns bytes written, 0 on error/insufficient. */
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
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        if (max < 3) return 0;
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

/* Decode one big-endian UTF-16 codepoint from guest EA. Returns u16s consumed, 0 on error. */
static int decode_utf16_guest(u32 ea, u32 len_units, u32* cp)
{
    if (len_units == 0) return 0;

    u16 w = vm_read16(ea);
    if (w >= 0xD800 && w <= 0xDBFF) {
        if (len_units < 2) return 0;
        u16 lo = vm_read16(ea + 2);
        if (lo < 0xDC00 || lo > 0xDFFF) return 0;
        *cp = 0x10000 + ((u32)(w - 0xD800) << 10) + (lo - 0xDC00);
        return 2;
    } else if (w >= 0xDC00 && w <= 0xDFFF) {
        return 0; /* lone low surrogate */
    }
    *cp = w;
    return 1;
}

/* Encode one codepoint to host-endian u16[2]. Returns u16s written, 0 on error. */
static int encode_utf16_units(u32 cp, u16* out, u32 max)
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
 * Single-character conversion (libl10n.sprx guest ABI)
 * -----------------------------------------------------------------------*/

s32 UTF8toUTF16(const u8* utf8, u16* utf16, u32* len)
{
    u32 src_ea = (u32)(uintptr_t)utf8;
    u32 dst_ea = (u32)(uintptr_t)utf16;
    u32 len_ea = (u32)(uintptr_t)len;
    if (!src_ea) return 0;

    u32 cp = 0;
    int consumed = decode_utf8(vm_base + src_ea, 4, &cp);
    if (consumed == 0) return 0;

    u16 units[2];
    int written = encode_utf16_units(cp, units, 2);
    if (written == 0) return 0;

    if (dst_ea) {
        for (int i = 0; i < written; i++)
            vm_write16(dst_ea + (u32)(2 * i), units[i]);
    }
    if (len_ea)
        vm_write32(len_ea, (u32)written);

    return (s32)consumed;
}

s32 UTF16toUTF8(const u16* utf16, u8* utf8, u32* len)
{
    u32 src_ea = (u32)(uintptr_t)utf16;
    u32 dst_ea = (u32)(uintptr_t)utf8;
    u32 len_ea = (u32)(uintptr_t)len;
    if (!src_ea) return 0;

    u32 cp = 0;
    int consumed = decode_utf16_guest(src_ea, 2, &cp);
    if (consumed == 0) return 0;

    u8 bytes[4];
    int written = encode_utf8(cp, bytes, 4);
    if (written == 0) return 0;

    if (dst_ea)
        memcpy(vm_base + dst_ea, bytes, (size_t)written);
    if (len_ea)
        vm_write32(len_ea, (u32)written);

    return (s32)consumed;
}

s32 UTF8toUTF32(const u8* utf8, u32* utf32)
{
    u32 src_ea = (u32)(uintptr_t)utf8;
    u32 dst_ea = (u32)(uintptr_t)utf32;
    if (!src_ea) return 0;

    u32 cp = 0;
    int consumed = decode_utf8(vm_base + src_ea, 4, &cp);
    if (consumed == 0) return 0;

    if (dst_ea)
        vm_write32(dst_ea, cp);
    return (s32)consumed;
}

s32 UTF32toUTF8(u32 utf32, u8* utf8)
{
    u32 dst_ea = (u32)(uintptr_t)utf8;
    u8 bytes[4];
    int written = encode_utf8(utf32, bytes, 4);
    if (written == 0) return 0;

    if (dst_ea)
        memcpy(vm_base + dst_ea, bytes, (size_t)written);
    return (s32)written;
}

s32 UTF16toUTF32(const u16* utf16, u32* utf32)
{
    u32 src_ea = (u32)(uintptr_t)utf16;
    u32 dst_ea = (u32)(uintptr_t)utf32;
    if (!src_ea) return 0;

    u32 cp = 0;
    int consumed = decode_utf16_guest(src_ea, 2, &cp);
    if (consumed == 0) return 0;

    if (dst_ea)
        vm_write32(dst_ea, cp);
    return (s32)consumed;
}

s32 UTF32toUTF16(u32 utf32, u16* utf16, u32* len)
{
    u32 dst_ea = (u32)(uintptr_t)utf16;
    u32 len_ea = (u32)(uintptr_t)len;
    u16 units[2];
    int written = encode_utf16_units(utf32, units, 2);
    if (written == 0) return 0;

    if (dst_ea) {
        for (int i = 0; i < written; i++)
            vm_write16(dst_ea + (u32)(2 * i), units[i]);
    }
    if (len_ea)
        vm_write32(len_ea, (u32)written);
    return (s32)written;
}

s32 UTF8toUCS2(const u8* utf8, u16* ucs2)
{
    u32 src_ea = (u32)(uintptr_t)utf8;
    u32 dst_ea = (u32)(uintptr_t)ucs2;
    if (!src_ea) return 0;

    u32 cp = 0;
    int consumed = decode_utf8(vm_base + src_ea, 4, &cp);
    if (consumed == 0 || cp > 0xFFFF) return 0;

    if (dst_ea)
        vm_write16(dst_ea, (u16)cp);
    return (s32)consumed;
}

s32 UCS2toUTF8(u16 ucs2, u8* utf8)
{
    u32 dst_ea = (u32)(uintptr_t)utf8;
    if (ucs2 >= 0xD800 && ucs2 <= 0xDFFF) return 0;

    u8 bytes[4];
    int written = encode_utf8((u32)ucs2, bytes, 4);
    if (written == 0) return 0;

    if (dst_ea)
        memcpy(vm_base + dst_ea, bytes, (size_t)written);
    return (s32)written;
}

/* ---------------------------------------------------------------------------
 * Generic UTF string conversion engine (guest EAs, BE lengths, dst==NULL sizing)
 * -----------------------------------------------------------------------*/

enum {
    L10N_UTF_K_UTF8  = 0,
    L10N_UTF_K_UTF16 = 1,
    L10N_UTF_K_UTF32 = 2,
    L10N_UTF_K_UCS2  = 3
};

static s32 l10n_conv_utf_guest(int sk, u32 src_ea, u32 src_len_ea,
                               int dk, u32 dst_ea, u32 dst_len_ea)
{
    if (!src_len_ea || !dst_len_ea)
        return CELL_L10N_SRC_ILLEGAL;

    u32 src_max = vm_read32(src_len_ea);
    u32 dst_max = dst_ea ? vm_read32(dst_len_ea) : 0xFFFFFFFFu;
    if (src_max > 0 && !src_ea)
        return CELL_L10N_SRC_ILLEGAL;

    u32 in_pos = 0;
    u32 out_pos = 0;
    s32 rc = CELL_L10N_CONVERSION_OK;

    while (in_pos < src_max) {
        u32 cp = 0;
        int consumed = 0;
        switch (sk) {
        case L10N_UTF_K_UTF8:
            consumed = decode_utf8(vm_base + src_ea + in_pos, src_max - in_pos, &cp);
            break;
        case L10N_UTF_K_UTF16:
            consumed = decode_utf16_guest(src_ea + 2 * in_pos, src_max - in_pos, &cp);
            break;
        case L10N_UTF_K_UTF32:
            cp = vm_read32(src_ea + 4 * in_pos);
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                consumed = 0;
            else
                consumed = 1;
            break;
        case L10N_UTF_K_UCS2: {
            u16 w = vm_read16(src_ea + 2 * in_pos);
            if (w >= 0xD800 && w <= 0xDFFF)
                consumed = 0;
            else {
                cp = w;
                consumed = 1;
            }
            break;
        }
        }
        if (consumed == 0) {
            rc = CELL_L10N_SRC_ILLEGAL;
            break;
        }

        u8  b8[4];
        u16 u16s[2];
        int written = 0;
        switch (dk) {
        case L10N_UTF_K_UTF8:
            written = encode_utf8(cp, b8, 4);
            break;
        case L10N_UTF_K_UTF16:
            written = encode_utf16_units(cp, u16s, 2);
            break;
        case L10N_UTF_K_UTF32:
            written = (cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) ? 1 : 0;
            break;
        case L10N_UTF_K_UCS2:
            if (cp <= 0xFFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) {
                u16s[0] = (u16)cp;
                written = 1;
            } else {
                written = 0;
            }
            break;
        }
        if (written == 0) {
            rc = CELL_L10N_SRC_ILLEGAL;
            break;
        }

        if (dst_ea) {
            if (out_pos + (u32)written > dst_max) {
                rc = CELL_L10N_DST_EXHAUSTED;
                break;
            }
            switch (dk) {
            case L10N_UTF_K_UTF8:
                memcpy(vm_base + dst_ea + out_pos, b8, (size_t)written);
                break;
            case L10N_UTF_K_UTF16:
            case L10N_UTF_K_UCS2:
                for (int i = 0; i < written; i++)
                    vm_write16(dst_ea + 2 * (out_pos + (u32)i), u16s[i]);
                break;
            case L10N_UTF_K_UTF32:
                vm_write32(dst_ea + 4 * out_pos, cp);
                break;
            }
        }

        in_pos += (u32)consumed;
        out_pos += (u32)written;
    }

    vm_write32(src_len_ea, in_pos);
    vm_write32(dst_len_ea, out_pos);
    return rc;
}

s32 UTF8stoUTF16s(const u8* utf8, u32* utf8_len,
                  u16* utf16, u32* utf16_len)
{
    return l10n_conv_utf_guest(L10N_UTF_K_UTF8,  (u32)(uintptr_t)utf8,  (u32)(uintptr_t)utf8_len,
                               L10N_UTF_K_UTF16, (u32)(uintptr_t)utf16, (u32)(uintptr_t)utf16_len);
}

s32 UTF16stoUTF8s(const u16* utf16, u32* utf16_len,
                  u8* utf8, u32* utf8_len)
{
    return l10n_conv_utf_guest(L10N_UTF_K_UTF16, (u32)(uintptr_t)utf16, (u32)(uintptr_t)utf16_len,
                               L10N_UTF_K_UTF8,  (u32)(uintptr_t)utf8,  (u32)(uintptr_t)utf8_len);
}

s32 UTF8stoUTF32s(const u8* utf8, u32* utf8_len,
                  u32* utf32, u32* utf32_len)
{
    return l10n_conv_utf_guest(L10N_UTF_K_UTF8,  (u32)(uintptr_t)utf8,  (u32)(uintptr_t)utf8_len,
                               L10N_UTF_K_UTF32, (u32)(uintptr_t)utf32, (u32)(uintptr_t)utf32_len);
}

s32 UTF32stoUTF8s(const u32* utf32, u32* utf32_len,
                  u8* utf8, u32* utf8_len)
{
    return l10n_conv_utf_guest(L10N_UTF_K_UTF32, (u32)(uintptr_t)utf32, (u32)(uintptr_t)utf32_len,
                               L10N_UTF_K_UTF8,  (u32)(uintptr_t)utf8,  (u32)(uintptr_t)utf8_len);
}

s32 UTF16stoUTF32s(const u16* utf16, u32* utf16_len,
                   u32* utf32, u32* utf32_len)
{
    return l10n_conv_utf_guest(L10N_UTF_K_UTF16, (u32)(uintptr_t)utf16, (u32)(uintptr_t)utf16_len,
                               L10N_UTF_K_UTF32, (u32)(uintptr_t)utf32, (u32)(uintptr_t)utf32_len);
}

s32 UTF32stoUTF16s(const u32* utf32, u32* utf32_len,
                   u16* utf16, u32* utf16_len)
{
    return l10n_conv_utf_guest(L10N_UTF_K_UTF32, (u32)(uintptr_t)utf32, (u32)(uintptr_t)utf32_len,
                               L10N_UTF_K_UTF16, (u32)(uintptr_t)utf16, (u32)(uintptr_t)utf16_len);
}

s32 UTF8stoUCS2s(const u8* utf8, u32* utf8_len,
                 u16* ucs2, u32* ucs2_len)
{
    return l10n_conv_utf_guest(L10N_UTF_K_UTF8, (u32)(uintptr_t)utf8, (u32)(uintptr_t)utf8_len,
                               L10N_UTF_K_UCS2, (u32)(uintptr_t)ucs2, (u32)(uintptr_t)ucs2_len);
}

s32 UCS2stoUTF8s(const u16* ucs2, u32* ucs2_len,
                 u8* utf8, u32* utf8_len)
{
    return l10n_conv_utf_guest(L10N_UTF_K_UCS2, (u32)(uintptr_t)ucs2, (u32)(uintptr_t)ucs2_len,
                               L10N_UTF_K_UTF8, (u32)(uintptr_t)utf8, (u32)(uintptr_t)utf8_len);
}

/* ---------------------------------------------------------------------------
 * Size queries (guest EAs, BE out_size)
 * -----------------------------------------------------------------------*/

s32 UTF8toUTF16Size(const u8* utf8, u32 utf8_len, u32* out_size)
{
    u32 src_ea = (u32)(uintptr_t)utf8;
    u32 out_ea = (u32)(uintptr_t)out_size;
    if (!src_ea || !out_ea)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    const u8* s = vm_base + src_ea;
    u32 count = 0;
    u32 pos = 0;

    while (pos < utf8_len) {
        u32 cp;
        int consumed = decode_utf8(s + pos, utf8_len - pos, &cp);
        if (consumed == 0)
            return CELL_L10N_SRC_ILLEGAL;
        pos += (u32)consumed;
        count += (cp >= 0x10000) ? 2 : 1;
    }

    vm_write32(out_ea, count);
    return CELL_L10N_CONVERSION_OK;
}

s32 UTF16toUTF8Size(const u16* utf16, u32 utf16_len, u32* out_size)
{
    u32 src_ea = (u32)(uintptr_t)utf16;
    u32 out_ea = (u32)(uintptr_t)out_size;
    if (!src_ea || !out_ea)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 count = 0;
    u32 pos = 0;

    while (pos < utf16_len) {
        u32 cp;
        int consumed = decode_utf16_guest(src_ea + 2 * pos, utf16_len - pos, &cp);
        if (consumed == 0)
            return CELL_L10N_SRC_ILLEGAL;
        pos += (u32)consumed;

        if (cp < 0x80) count += 1;
        else if (cp < 0x800) count += 2;
        else if (cp < 0x10000) count += 3;
        else count += 4;
    }

    vm_write32(out_ea, count);
    return CELL_L10N_CONVERSION_OK;
}

/* ---------------------------------------------------------------------------
 * Codepoint length queries (guest EAs)
 * -----------------------------------------------------------------------*/

s32 UTF8Len(const u8* utf8)
{
    utf8 = GUEST_PTR(utf8, const u8*);
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
    u32 ea = (u32)(uintptr_t)utf16;
    if (!ea) return 0;

    u16 w = vm_read16(ea);
    if (w >= 0xD800 && w <= 0xDBFF) return 2; /* surrogate pair */
    return 1;
}

/* ---------------------------------------------------------------------------
 * Generic converter interface (guest EAs, BE out-params)
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
    return CELL_L10N_CONVERTER_UNKNOWN;
}

/* Internal byte-oriented guest converter helper */
static s32 l10n_convert_guest(u32 src_encoding, u32 src_ea, u32 src_len,
                              u32 dst_encoding, u32 dst_ea, u32 dst_max,
                              u32* out_dst_bytes)
{
    if (!src_ea || !dst_ea || !out_dst_bytes)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    /* Same encoding or ASCII -> UTF-8 */
    if (src_encoding == dst_encoding ||
        (src_encoding == L10N_ASCII && dst_encoding == L10N_UTF8)) {
        u32 copy_len = src_len < dst_max ? src_len : dst_max;
        if (copy_len)
            memcpy(vm_base + dst_ea, vm_base + src_ea, copy_len);
        *out_dst_bytes = copy_len;
        return (src_len > dst_max) ? CELL_L10N_DST_EXHAUSTED : CELL_L10N_CONVERSION_OK;
    }

    /* ISO-8859-1 -> UTF-8 */
    if (src_encoding == L10N_ISO_8859_1 && dst_encoding == L10N_UTF8) {
        const u8* s = vm_base + src_ea;
        u8* d = vm_base + dst_ea;
        u32 out_pos = 0;
        for (u32 i = 0; i < src_len; i++) {
            int w = encode_utf8(s[i], d + out_pos, dst_max - out_pos);
            if (w == 0) {
                *out_dst_bytes = out_pos;
                return CELL_L10N_DST_EXHAUSTED;
            }
            out_pos += (u32)w;
        }
        *out_dst_bytes = out_pos;
        return CELL_L10N_CONVERSION_OK;
    }

    /* ISO-8859-1 -> UTF-16 (big-endian) */
    if (src_encoding == L10N_ISO_8859_1 && dst_encoding == L10N_UTF16) {
        const u8* s = vm_base + src_ea;
        u32 max_units = dst_max / sizeof(u16);
        u32 count = src_len < max_units ? src_len : max_units;
        for (u32 i = 0; i < count; i++)
            vm_write16(dst_ea + 2 * i, (u16)s[i]);
        *out_dst_bytes = count * (u32)sizeof(u16);
        return (src_len > max_units) ? CELL_L10N_DST_EXHAUSTED : CELL_L10N_CONVERSION_OK;
    }

    /* UTF-family conversions (src_len / dst_max are in bytes) */
    if (src_encoding <= L10N_UCS2 && dst_encoding <= L10N_UCS2) {
        static const u32 kUnitSize[4] = { 1, 2, 4, 2 };
        u32 s_unit = kUnitSize[src_encoding];
        u32 d_unit = kUnitSize[dst_encoding];
        u32 src_units = src_len / s_unit;
        u32 dst_units_max = dst_max / d_unit;
        u32 in_pos = 0, out_pos = 0;

        while (in_pos < src_units) {
            u32 cp = 0;
            int consumed = 0;
            if (src_encoding == L10N_UTF8)
                consumed = decode_utf8(vm_base + src_ea + in_pos, src_units - in_pos, &cp);
            else if (src_encoding == L10N_UTF16)
                consumed = decode_utf16_guest(src_ea + 2 * in_pos, src_units - in_pos, &cp);
            else if (src_encoding == L10N_UTF32) {
                cp = vm_read32(src_ea + 4 * in_pos);
                consumed = (cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) ? 1 : 0;
            } else {
                u16 w = vm_read16(src_ea + 2 * in_pos);
                consumed = (w >= 0xD800 && w <= 0xDFFF) ? 0 : 1;
                cp = w;
            }
            if (consumed == 0) {
                *out_dst_bytes = out_pos * d_unit;
                return CELL_L10N_SRC_ILLEGAL;
            }

            u8 b8[4];
            u16 u16s[2];
            int written = 0;
            if (dst_encoding == L10N_UTF8)
                written = encode_utf8(cp, b8, 4);
            else if (dst_encoding == L10N_UTF16)
                written = encode_utf16_units(cp, u16s, 2);
            else if (dst_encoding == L10N_UTF32)
                written = 1;
            else if (cp <= 0xFFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) {
                u16s[0] = (u16)cp;
                written = 1;
            }
            if (written == 0) {
                *out_dst_bytes = out_pos * d_unit;
                return CELL_L10N_SRC_ILLEGAL;
            }
            if (out_pos + (u32)written > dst_units_max) {
                *out_dst_bytes = out_pos * d_unit;
                return CELL_L10N_DST_EXHAUSTED;
            }
            if (dst_encoding == L10N_UTF8)
                memcpy(vm_base + dst_ea + out_pos, b8, (size_t)written);
            else if (dst_encoding == L10N_UTF16 || dst_encoding == L10N_UCS2) {
                for (int i = 0; i < written; i++)
                    vm_write16(dst_ea + 2 * (out_pos + (u32)i), u16s[i]);
            } else {
                vm_write32(dst_ea + 4 * out_pos, cp);
            }
            in_pos += (u32)consumed;
            out_pos += (u32)written;
        }
        *out_dst_bytes = out_pos * d_unit;
        return CELL_L10N_CONVERSION_OK;
    }

    printf("[cellL10n] convert: unsupported %u -> %u\n",
           src_encoding, dst_encoding);
    return CELL_L10N_CONVERTER_UNKNOWN;
}

s32 l10n_convert(u32 src_encoding, const void* src, u32 src_len,
                 u32 dst_encoding, void* dst, u32 dst_max, u32* dst_len)
{
    u32 dst_len_ea = (u32)(uintptr_t)dst_len;
    if (!dst_len_ea)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 out_bytes = 0;
    s32 rc = l10n_convert_guest(src_encoding, (u32)(uintptr_t)src, src_len,
                                dst_encoding, (u32)(uintptr_t)dst, dst_max,
                                &out_bytes);
    if (rc == CELL_L10N_CONVERSION_OK || rc == CELL_L10N_DST_EXHAUSTED)
        vm_write32(dst_len_ea, out_bytes);
    return rc;
}

s32 l10n_convert_str(s32 src_code, const void* src, u32* src_len,
                     s32 dst_code, void* dst, u32* dst_len)
{
    u32 src_len_ea = (u32)(uintptr_t)src_len;
    u32 dst_len_ea = (u32)(uintptr_t)dst_len;
    if (!src_len_ea || !dst_len_ea)
        return (s32)CELL_L10N_ERROR_INVALID_PARAM;

    u32 s_len = vm_read32(src_len_ea);
    u32 d_max = vm_read32(dst_len_ea);
    u32 out_bytes = 0;
    s32 rc = l10n_convert_guest((u32)src_code, (u32)(uintptr_t)src, s_len,
                                (u32)dst_code, (u32)(uintptr_t)dst, d_max,
                                &out_bytes);
    if (rc == CELL_L10N_CONVERSION_OK || rc == CELL_L10N_DST_EXHAUSTED)
        vm_write32(dst_len_ea, out_bytes);
    return rc;
}

/* ---------------------------------------------------------------------------
 * Shift-JIS string conversions, against GUEST memory
 *
 * Follows libl10n.sprx _L10nConvertStr: *src_len units are converted (NULs
 * included); with no dst the size is reported; a dst too small is DSTExhausted.
 * Results: 0 ConversionOK, 1 SRCIllegal, 2 DSTExhausted, 3 ConverterUnknown.
 *
 * Shift-JIS is code page 932 (a superset), decoded by the host on Windows;
 * elsewhere only the ASCII subset converts until a table is added.
 * -----------------------------------------------------------------------*/

enum { L10N_K_SJIS, L10N_K_UTF8, L10N_K_UCS2 };

/* src -> host UTF-16 (malloc'd), returns unit count or -1 on bad input */
static int l10n_to_wide(int kind, u32 src, u32 n, wchar_t** out)
{
    wchar_t* w = (wchar_t*)malloc(sizeof(wchar_t) * (n + 1));
    if (!w) return -1;
    int len = 0;
    if (kind == L10N_K_UCS2) {
        for (u32 i = 0; i < n; i++) w[len++] = (wchar_t)vm_read16(src + 2 * i);
    } else if (n) {
#ifdef _WIN32
        len = MultiByteToWideChar(kind == L10N_K_SJIS ? 932 : CP_UTF8, MB_ERR_INVALID_CHARS,
                                  (const char*)(vm_base + src), (int)n, w, (int)n);
        if (len <= 0) { free(w); return -1; }
#else
        for (u32 i = 0; i < n; i++) {
            u8 c = vm_base[src + i];
            if (c >= 0x80) { free(w); return -1; }
            w[len++] = c;
        }
#endif
    }
    *out = w;
    return len;
}

static s32 l10n_conv_guest(int sk, u32 src, u32 src_len_ea, int dk, u32 dst, u32 dst_len_ea)
{
    if (!src_len_ea || !dst_len_ea) return (s32)CELL_L10N_ERROR_INVALID_PARAM;
    const u32 n = vm_read32(src_len_ea);
    wchar_t* w = NULL;
    const int wl = l10n_to_wide(sk, src, n, &w);
    if (wl < 0) return CELL_L10N_SRC_ILLEGAL;

    /* host UTF-16 -> dst kind, sized first */
    u32 need = 0;
    char* bytes = NULL;
    if (dk == L10N_K_UCS2) {
        need = (u32)wl;
    } else if (wl) {
#ifdef _WIN32
        const UINT cp = dk == L10N_K_SJIS ? 932 : CP_UTF8;
        const int bl = WideCharToMultiByte(cp, 0, w, wl, NULL, 0, NULL, NULL);
        if (bl <= 0) { free(w); return CELL_L10N_SRC_ILLEGAL; }
        bytes = (char*)malloc((size_t)bl);
        if (!bytes) { free(w); return (s32)CELL_L10N_ERROR_INVALID_PARAM; }
        WideCharToMultiByte(cp, 0, w, wl, bytes, bl, NULL, NULL);
        need = (u32)bl;
#else
        bytes = (char*)malloc((size_t)wl);
        for (int i = 0; i < wl; i++) {
            if (w[i] >= 0x80) { free(bytes); free(w); return CELL_L10N_SRC_ILLEGAL; }
            bytes[i] = (char)w[i];
        }
        need = (u32)wl;
#endif
    }

    s32 rc = CELL_L10N_CONVERSION_OK;
    if (dst) {
        if (need > vm_read32(dst_len_ea)) rc = CELL_L10N_DST_EXHAUSTED;
        else if (dk == L10N_K_UCS2)
            for (u32 i = 0; i < need; i++) vm_write16(dst + 2 * i, (u16)w[i]);
        else if (need)
            memcpy(vm_base + dst, bytes, need);
    }
    if (rc == CELL_L10N_CONVERSION_OK) vm_write32(dst_len_ea, need);
    {   static int s_logs = 0;
        if (s_logs < 16) {
            s_logs++;
            printf("[cellL10n] convert %d->%d src=0x%08X len=%u dst=0x%08X -> %u units, rc=%d\n",
                   sk, dk, src, n, dst, need, rc);
        }
    }
    free(bytes);
    free(w);
    return rc;
}

s32 SJISstoUTF8s(u32 src, u32 src_len, u32 dst, u32 dst_len)
{ return l10n_conv_guest(L10N_K_SJIS, src, src_len, L10N_K_UTF8, dst, dst_len); }
s32 UTF8stoSJISs(u32 src, u32 src_len, u32 dst, u32 dst_len)
{ return l10n_conv_guest(L10N_K_UTF8, src, src_len, L10N_K_SJIS, dst, dst_len); }
s32 SJISstoUCS2s(u32 src, u32 src_len, u32 dst, u32 dst_len)
{ return l10n_conv_guest(L10N_K_SJIS, src, src_len, L10N_K_UCS2, dst, dst_len); }
s32 UCS2stoSJISs(u32 src, u32 src_len, u32 dst, u32 dst_len)
{ return l10n_conv_guest(L10N_K_UCS2, src, src_len, L10N_K_SJIS, dst, dst_len); }

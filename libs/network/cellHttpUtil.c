/*
 * ps3recomp - cellHttpUtil HLE implementation
 *
 * URL parsing, percent-encoding, and Base64 codec. Pure C, no
 * external dependencies.
 */

#include "cellHttpUtil.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static const char s_hex[] = "0123456789ABCDEF";

/* Is a character "unreserved" per RFC 3986? */
static int is_unreserved(u8 c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return 1;
    return (c == '-' || c == '_' || c == '.' || c == '~');
}

/* ---------------------------------------------------------------------------
 * URL parsing
 * -----------------------------------------------------------------------*/

s32 cellHttpUtilParseUri(CellHttpUtilUri* uri, const char* url,
                          void* pool, u32 poolSize, u32* required)
{
    (void)pool;
    (void)poolSize;
    (void)required;

    if (!uri || !url)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    memset(uri, 0, sizeof(CellHttpUtilUri));
    uri->port = 0;

    const char* p = url;

    /* Scheme (http, https, etc.) */
    const char* colon = strstr(p, "://");
    if (colon) {
        u32 scheme_len = (u32)(colon - p);
        if (scheme_len >= sizeof(uri->scheme))
            scheme_len = sizeof(uri->scheme) - 1;
        memcpy(uri->scheme, p, scheme_len);
        uri->scheme[scheme_len] = '\0';
        p = colon + 3;
    }

    /* User info (user:pass@) */
    const char* at = strchr(p, '@');
    const char* slash = strchr(p, '/');
    if (at && (!slash || at < slash)) {
        const char* user_colon = strchr(p, ':');
        if (user_colon && user_colon < at) {
            u32 ulen = (u32)(user_colon - p);
            if (ulen >= sizeof(uri->username)) ulen = sizeof(uri->username) - 1;
            memcpy(uri->username, p, ulen);

            u32 plen = (u32)(at - user_colon - 1);
            if (plen >= sizeof(uri->password)) plen = sizeof(uri->password) - 1;
            memcpy(uri->password, user_colon + 1, plen);
        } else {
            u32 ulen = (u32)(at - p);
            if (ulen >= sizeof(uri->username)) ulen = sizeof(uri->username) - 1;
            memcpy(uri->username, p, ulen);
        }
        p = at + 1;
    }

    /* Hostname and port */
    const char* host_end = p;
    while (*host_end && *host_end != '/' && *host_end != '?' && *host_end != '#')
        host_end++;

    /* Check for port */
    const char* port_colon = NULL;
    for (const char* c = p; c < host_end; c++) {
        if (*c == ':') port_colon = c;
    }

    if (port_colon) {
        u32 hlen = (u32)(port_colon - p);
        if (hlen >= sizeof(uri->hostname)) hlen = sizeof(uri->hostname) - 1;
        memcpy(uri->hostname, p, hlen);

        uri->port = 0;
        for (const char* c = port_colon + 1; c < host_end; c++)
            uri->port = uri->port * 10 + (*c - '0');
    } else {
        u32 hlen = (u32)(host_end - p);
        if (hlen >= sizeof(uri->hostname)) hlen = sizeof(uri->hostname) - 1;
        memcpy(uri->hostname, p, hlen);

        /* Default ports */
        if (strcmp(uri->scheme, "https") == 0)
            uri->port = 443;
        else if (strcmp(uri->scheme, "http") == 0)
            uri->port = 80;
    }

    p = host_end;

    /* Path */
    if (*p == '/') {
        const char* path_end = p;
        while (*path_end && *path_end != '?' && *path_end != '#')
            path_end++;
        u32 plen = (u32)(path_end - p);
        if (plen >= sizeof(uri->path)) plen = sizeof(uri->path) - 1;
        memcpy(uri->path, p, plen);
        p = path_end;
    } else {
        strcpy(uri->path, "/");
    }

    /* Query */
    if (*p == '?') {
        p++;
        const char* q_end = strchr(p, '#');
        if (!q_end) q_end = p + strlen(p);
        u32 qlen = (u32)(q_end - p);
        if (qlen >= sizeof(uri->query)) qlen = sizeof(uri->query) - 1;
        memcpy(uri->query, p, qlen);
        p = q_end;
    }

    /* Fragment */
    if (*p == '#') {
        p++;
        u32 flen = (u32)strlen(p);
        if (flen >= sizeof(uri->fragment)) flen = sizeof(uri->fragment) - 1;
        memcpy(uri->fragment, p, flen);
    }

    return CELL_OK;
}

s32 cellHttpUtilBuildUri(char* urlBuf, u32 urlBufSize,
                          const CellHttpUtilUri* uri, u32* written)
{
    if (!urlBuf || !uri)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    int n = snprintf(urlBuf, urlBufSize, "%s://%s",
                     uri->scheme[0] ? uri->scheme : "http",
                     uri->hostname);
    if (n < 0) return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;

    /* Append port if non-default */
    int default_port = (strcmp(uri->scheme, "https") == 0) ? 443 : 80;
    if (uri->port && uri->port != (u32)default_port) {
        n += snprintf(urlBuf + n, urlBufSize - n, ":%u", uri->port);
    }

    /* Path */
    n += snprintf(urlBuf + n, urlBufSize - n, "%s",
                  uri->path[0] ? uri->path : "/");

    /* Query */
    if (uri->query[0])
        n += snprintf(urlBuf + n, urlBufSize - n, "?%s", uri->query);

    /* Fragment */
    if (uri->fragment[0])
        n += snprintf(urlBuf + n, urlBufSize - n, "#%s", uri->fragment);

    if (written)
        *written = (u32)n;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * URL encoding / decoding
 * -----------------------------------------------------------------------*/

s32 cellHttpUtilEscapeUri(char* out, u32 outSize,
                           const u8* src, u32 srcSize, u32* written)
{
    if (!out || !src)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 pos = 0;
    for (u32 i = 0; i < srcSize; i++) {
        if (is_unreserved(src[i])) {
            if (pos >= outSize)
                return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
            out[pos++] = (char)src[i];
        } else {
            if (pos + 3 > outSize)
                return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
            out[pos++] = '%';
            out[pos++] = s_hex[(src[i] >> 4) & 0xF];
            out[pos++] = s_hex[src[i] & 0xF];
        }
    }

    if (pos < outSize)
        out[pos] = '\0';

    if (written)
        *written = pos;

    return CELL_OK;
}

s32 cellHttpUtilUnescapeUri(u8* out, u32 outSize,
                             const char* src, u32 srcSize, u32* written)
{
    if (!out || !src)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 pos = 0;
    for (u32 i = 0; i < srcSize; i++) {
        if (pos >= outSize)
            return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;

        if (src[i] == '%' && i + 2 < srcSize) {
            int hi = hex_digit(src[i + 1]);
            int lo = hex_digit(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[pos++] = (u8)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[pos++] = (u8)src[i];
    }

    if (written)
        *written = pos;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Form URL encoding
 * -----------------------------------------------------------------------*/

s32 cellHttpUtilFormUrlEncode(char* out, u32 outSize,
                               const char* key, const char* value, u32* written)
{
    if (!out || !key || !value)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 key_esc_len = outSize;
    u32 pos = 0;

    /* Encode key */
    s32 rc = cellHttpUtilEscapeUri(out, outSize, (const u8*)key,
                                    (u32)strlen(key), &key_esc_len);
    if (rc != CELL_OK) return rc;
    pos = key_esc_len;

    /* = */
    if (pos >= outSize)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
    out[pos++] = '=';

    /* Encode value */
    u32 val_esc_len = outSize - pos;
    rc = cellHttpUtilEscapeUri(out + pos, outSize - pos, (const u8*)value,
                                (u32)strlen(value), &val_esc_len);
    if (rc != CELL_OK) return rc;
    pos += val_esc_len;

    if (pos < outSize)
        out[pos] = '\0';

    if (written)
        *written = pos;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Base64
 * -----------------------------------------------------------------------*/

static const char s_b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

s32 cellHttpUtilBase64Encode(char* out, u32 outSize,
                              const u8* data, u32 dataSize, u32* written)
{
    if (!out || !data)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 needed = ((dataSize + 2) / 3) * 4;
    if (needed >= outSize)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;

    u32 pos = 0;
    for (u32 i = 0; i < dataSize; i += 3) {
        u32 n = ((u32)data[i]) << 16;
        if (i + 1 < dataSize) n |= ((u32)data[i + 1]) << 8;
        if (i + 2 < dataSize) n |= data[i + 2];

        out[pos++] = s_b64_table[(n >> 18) & 0x3F];
        out[pos++] = s_b64_table[(n >> 12) & 0x3F];
        out[pos++] = (i + 1 < dataSize) ? s_b64_table[(n >> 6) & 0x3F] : '=';
        out[pos++] = (i + 2 < dataSize) ? s_b64_table[n & 0x3F] : '=';
    }

    out[pos] = '\0';
    if (written)
        *written = pos;

    return CELL_OK;
}

static int b64_decode_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

s32 cellHttpUtilBase64Decode(u8* out, u32 outSize,
                              const char* encoded, u32 encodedLen, u32* written)
{
    if (!out || !encoded)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 pos = 0;
    for (u32 i = 0; i < encodedLen; i += 4) {
        int a = (i < encodedLen) ? b64_decode_char(encoded[i]) : 0;
        int b = (i + 1 < encodedLen) ? b64_decode_char(encoded[i + 1]) : 0;
        int c = (i + 2 < encodedLen) ? b64_decode_char(encoded[i + 2]) : -1;
        int d = (i + 3 < encodedLen) ? b64_decode_char(encoded[i + 3]) : -1;

        if (a < 0 || b < 0)
            return (s32)CELL_HTTP_UTIL_ERROR_PARSE_FAILED;

        u32 triple = ((u32)a << 18) | ((u32)b << 12);
        if (c >= 0) triple |= ((u32)c << 6);
        if (d >= 0) triple |= (u32)d;

        if (pos >= outSize) return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
        out[pos++] = (u8)((triple >> 16) & 0xFF);

        if (c >= 0 && pos < outSize)
            out[pos++] = (u8)((triple >> 8) & 0xFF);

        if (d >= 0 && pos < outSize)
            out[pos++] = (u8)(triple & 0xFF);
    }

    if (written)
        *written = pos;

    return CELL_OK;
}

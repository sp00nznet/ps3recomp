/*
 * ps3recomp - cellHttpUtil HLE
 *
 * HTTP utility functions: URL parsing, encoding, cookie management,
 * and header manipulation.
 */

#ifndef PS3RECOMP_CELL_HTTP_UTIL_H
#define PS3RECOMP_CELL_HTTP_UTIL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_HTTP_UTIL_ERROR_INVALID_PARAM  0x80711001
#define CELL_HTTP_UTIL_ERROR_NO_MEMORY      0x80711002
#define CELL_HTTP_UTIL_ERROR_NO_BUFFER      0x80711003
#define CELL_HTTP_UTIL_ERROR_PARSE_FAILED   0x80711004

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

typedef struct CellHttpUtilUri {
    char scheme[16];
    char hostname[256];
    char path[1024];
    char query[1024];
    char fragment[256];
    char username[64];
    char password[64];
    u32  port;
} CellHttpUtilUri;

/* ---------------------------------------------------------------------------
 * URL parsing
 * -----------------------------------------------------------------------*/

s32 cellHttpUtilParseUri(CellHttpUtilUri* uri, const char* url,
                          void* pool, u32 poolSize, u32* required);
s32 cellHttpUtilBuildUri(char* urlBuf, u32 urlBufSize,
                          const CellHttpUtilUri* uri, u32* written);

/* ---------------------------------------------------------------------------
 * URL encoding / decoding
 * -----------------------------------------------------------------------*/

s32 cellHttpUtilEscapeUri(char* out, u32 outSize,
                           const u8* src, u32 srcSize, u32* written);
s32 cellHttpUtilUnescapeUri(u8* out, u32 outSize,
                             const char* src, u32 srcSize, u32* written);

/* ---------------------------------------------------------------------------
 * Header helpers
 * -----------------------------------------------------------------------*/

s32 cellHttpUtilFormUrlEncode(char* out, u32 outSize,
                               const char* key, const char* value, u32* written);

/* Base64 encode/decode (used for HTTP Basic auth) */
s32 cellHttpUtilBase64Encode(char* out, u32 outSize,
                              const u8* data, u32 dataSize, u32* written);
s32 cellHttpUtilBase64Decode(u8* out, u32 outSize,
                              const char* encoded, u32 encodedLen, u32* written);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_HTTP_UTIL_H */

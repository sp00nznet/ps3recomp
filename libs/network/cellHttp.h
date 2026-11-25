/*
 * ps3recomp - cellHttp HLE
 *
 * HTTP client: create connections, send requests, receive responses.
 */

#ifndef PS3RECOMP_CELL_HTTP_H
#define PS3RECOMP_CELL_HTTP_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes (PS3 cell/http.h, base 0x80710000; verified against RPCS3)
 * -----------------------------------------------------------------------*/
#define CELL_HTTP_ERROR_NOT_INITIALIZED         (CELL_ERROR_BASE_HTTP | 0x02) /* 0x80710002 */
#define CELL_HTTP_ERROR_ALREADY_INITIALIZED     (CELL_ERROR_BASE_HTTP | 0x01) /* 0x80710001 */
#define CELL_HTTP_ERROR_NO_MEMORY               (CELL_ERROR_BASE_HTTP | 0x03)
#define CELL_HTTP_ERROR_NOT_FOUND               (CELL_ERROR_BASE_HTTP | 0x04)
#define CELL_HTTP_ERROR_INVALID_PARAMETER       (CELL_ERROR_BASE_HTTP | 0x05)
#define CELL_HTTP_ERROR_NO_BUFFER               (CELL_ERROR_BASE_HTTP | 0x06)
#define CELL_HTTP_ERROR_CONNECTION_FAILED       (CELL_ERROR_BASE_HTTP | 0x07)
#define CELL_HTTP_ERROR_SEND_FAILED             (CELL_ERROR_BASE_HTTP | 0x08)
#define CELL_HTTP_ERROR_RECV_FAILED             (CELL_ERROR_BASE_HTTP | 0x09)
#define CELL_HTTP_ERROR_TIMEOUT                 (CELL_ERROR_BASE_HTTP | 0x0A)
#define CELL_HTTP_ERROR_INVALID_URL             (CELL_ERROR_BASE_HTTP | 0x0B)
#define CELL_HTTP_ERROR_ABORTED                 (CELL_ERROR_BASE_HTTP | 0x0C)
#define CELL_HTTP_ERROR_UNKNOWN                 (CELL_ERROR_BASE_HTTP | 0xFF)
#define CELL_HTTP_ERROR_NO_CONTENT_LENGTH       (CELL_ERROR_BASE_HTTP | 0x17) /* 0x80710017 */

/* ---------------------------------------------------------------------------
 * HTTP methods
 * -----------------------------------------------------------------------*/
#define CELL_HTTP_METHOD_GET        "GET"
#define CELL_HTTP_METHOD_POST       "POST"
#define CELL_HTTP_METHOD_HEAD       "HEAD"
#define CELL_HTTP_METHOD_PUT        "PUT"
#define CELL_HTTP_METHOD_DELETE     "DELETE"

/* ---------------------------------------------------------------------------
 * Status codes
 * -----------------------------------------------------------------------*/
#define CELL_HTTP_STATUS_OK                 200
#define CELL_HTTP_STATUS_CREATED            201
#define CELL_HTTP_STATUS_NO_CONTENT         204
#define CELL_HTTP_STATUS_MOVED_PERMANENTLY  301
#define CELL_HTTP_STATUS_FOUND              302
#define CELL_HTTP_STATUS_NOT_MODIFIED       304
#define CELL_HTTP_STATUS_BAD_REQUEST        400
#define CELL_HTTP_STATUS_UNAUTHORIZED       401
#define CELL_HTTP_STATUS_FORBIDDEN          403
#define CELL_HTTP_STATUS_NOT_FOUND          404
#define CELL_HTTP_STATUS_INTERNAL_ERROR     500
#define CELL_HTTP_STATUS_SERVICE_UNAVAIL    503

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef u32 CellHttpClientId;
typedef u32 CellHttpTransId;

/* Guest layout (4-byte EAs): scheme +0, hostname +4, username +8,
 * password +12, path +16, port +20, reserved +24. cellHttp.c reads it by
 * guest offset; this host declaration only names the fields in order. */
typedef struct CellHttpUri {
    const char* scheme;
    const char* hostname;
    const char* username;
    const char* password;
    const char* path;
    u32         port;
    u8          reserved[4];
} CellHttpUri;

/* Max number of concurrent clients and transactions */
#define CELL_HTTP_MAX_CLIENTS       8
#define CELL_HTTP_MAX_TRANSACTIONS  32

/* Max custom request headers per transaction */
#define CELL_HTTP_MAX_CUSTOM_HEADERS 16

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellHttpInit(void* pool, u32 poolSize);
s32 cellHttpEnd(void);

s32 cellHttpCreateClient(CellHttpClientId* clientId);
s32 cellHttpDestroyClient(CellHttpClientId clientId);

s32 cellHttpCreateTransaction(CellHttpTransId* transId, CellHttpClientId clientId,
                              const char* method, const CellHttpUri* uri);
s32 cellHttpDestroyTransaction(CellHttpTransId transId);

s32 cellHttpSendRequest(CellHttpTransId transId, const void* buf, u32 size,
                        u32* sent);
s32 cellHttpRecvResponse(CellHttpTransId transId, void* buf, u32 size,
                         u32* received);

s32 cellHttpGetResponseContentLength(CellHttpTransId transId, u64* length);
/* SDK names (CellHttpHeader** headers; size_t items/poolSize/required are 32-bit guest values) */
s32 cellHttpResponseGetStatusCode(CellHttpTransId transId, s32* code);
s32 cellHttpResponseGetContentLength(CellHttpTransId transId, u64* length);
s32 cellHttpResponseGetAllHeaders(CellHttpTransId transId, void* headers, u32* items, void* pool, u32 poolSize,
                                  u32* required);
s32 cellHttpGetStatusCode(CellHttpTransId transId, s32* code);

s32 cellHttpSetResolveTimeOut(CellHttpTransId transId, u32 usec);
s32 cellHttpSetConnectTimeOut(CellHttpTransId transId, u32 usec);
s32 cellHttpSetSendTimeOut(CellHttpTransId transId, u32 usec);
s32 cellHttpSetRecvTimeOut(CellHttpTransId transId, u32 usec);

s32 cellHttpSetRequestContentLength(CellHttpTransId transId, u64 length);
s32 cellHttpAddRequestHeader(CellHttpTransId transId, const char* name,
                             const char* value);
s32 cellHttpAbortTransaction(CellHttpTransId transId);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_HTTP_H */

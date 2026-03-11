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
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_HTTP_ERROR_NOT_INITIALIZED         0x80710101
#define CELL_HTTP_ERROR_ALREADY_INITIALIZED     0x80710102
#define CELL_HTTP_ERROR_NO_MEMORY               0x80710103
#define CELL_HTTP_ERROR_NOT_FOUND               0x80710104
#define CELL_HTTP_ERROR_INVALID_PARAMETER       0x80710105
#define CELL_HTTP_ERROR_NO_BUFFER               0x80710106
#define CELL_HTTP_ERROR_CONNECTION_FAILED       0x80710107
#define CELL_HTTP_ERROR_SEND_FAILED             0x80710108
#define CELL_HTTP_ERROR_RECV_FAILED             0x80710109
#define CELL_HTTP_ERROR_TIMEOUT                 0x8071010A
#define CELL_HTTP_ERROR_INVALID_URL             0x8071010B
#define CELL_HTTP_ERROR_ABORTED                 0x8071010C
#define CELL_HTTP_ERROR_UNKNOWN                 0x807101FF

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

typedef struct CellHttpUri {
    const char* scheme;
    const char* hostname;
    const char* path;
    const char* username;
    const char* password;
    u32         port;
} CellHttpUri;

/* Max number of concurrent clients and transactions */
#define CELL_HTTP_MAX_CLIENTS       8
#define CELL_HTTP_MAX_TRANSACTIONS  32

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellHttpInit(u32 poolSize);
s32 cellHttpEnd(void);

s32 cellHttpCreateClient(CellHttpClientId* clientId);
s32 cellHttpDestroyClient(CellHttpClientId clientId);

s32 cellHttpCreateTransaction(CellHttpClientId clientId, const char* method,
                              const CellHttpUri* uri, CellHttpTransId* transId);
s32 cellHttpDestroyTransaction(CellHttpTransId transId);

s32 cellHttpSendRequest(CellHttpTransId transId, const void* buf, u32 size,
                        u32* sent);
s32 cellHttpRecvResponse(CellHttpTransId transId, void* buf, u32 size,
                         u32* received);

s32 cellHttpGetResponseContentLength(CellHttpTransId transId, u64* length);
s32 cellHttpGetStatusCode(CellHttpTransId transId, s32* code);

s32 cellHttpSetResolveTimeOut(CellHttpTransId transId, u32 usec);
s32 cellHttpSetConnectTimeOut(CellHttpTransId transId, u32 usec);
s32 cellHttpSetSendTimeOut(CellHttpTransId transId, u32 usec);
s32 cellHttpSetRecvTimeOut(CellHttpTransId transId, u32 usec);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_HTTP_H */

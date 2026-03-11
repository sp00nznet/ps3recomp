/*
 * ps3recomp - cellHttp HLE implementation
 *
 * Provides HTTP client stubs.  Requests are logged but not actually sent.
 * Games that check HTTP responses will see a connection-failed error,
 * which is the safest stub behavior (most games handle network failure).
 *
 * TODO: Full implementation using Winsock/BSD sockets or libcurl.
 */

#include "cellHttp.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_http_initialized = 0;

typedef struct {
    int                  in_use;
    u32                  resolve_timeout;
    u32                  connect_timeout;
    u32                  send_timeout;
    u32                  recv_timeout;
} HttpClientSlot;

typedef struct {
    int                  in_use;
    CellHttpClientId     client;
    char                 method[16];
    char                 url[1024];
    s32                  status_code;
    u64                  content_length;
} HttpTransSlot;

static HttpClientSlot s_clients[CELL_HTTP_MAX_CLIENTS];
static HttpTransSlot  s_transactions[CELL_HTTP_MAX_TRANSACTIONS];

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellHttpInit(u32 poolSize)
{
    printf("[cellHttp] Init(poolSize=%u)\n", poolSize);

    if (s_http_initialized)
        return CELL_HTTP_ERROR_ALREADY_INITIALIZED;

    memset(s_clients, 0, sizeof(s_clients));
    memset(s_transactions, 0, sizeof(s_transactions));
    s_http_initialized = 1;
    return CELL_OK;
}

s32 cellHttpEnd(void)
{
    printf("[cellHttp] End()\n");

    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    memset(s_clients, 0, sizeof(s_clients));
    memset(s_transactions, 0, sizeof(s_transactions));
    s_http_initialized = 0;
    return CELL_OK;
}

s32 cellHttpCreateClient(CellHttpClientId* clientId)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!clientId)
        return CELL_HTTP_ERROR_INVALID_PARAMETER;

    for (u32 i = 0; i < CELL_HTTP_MAX_CLIENTS; i++) {
        if (!s_clients[i].in_use) {
            s_clients[i].in_use = 1;
            s_clients[i].resolve_timeout = 30000000; /* 30s default */
            s_clients[i].connect_timeout = 30000000;
            s_clients[i].send_timeout    = 120000000;
            s_clients[i].recv_timeout    = 120000000;
            *clientId = i;
            printf("[cellHttp] CreateClient(id=%u)\n", i);
            return CELL_OK;
        }
    }

    return CELL_HTTP_ERROR_NO_MEMORY;
}

s32 cellHttpDestroyClient(CellHttpClientId clientId)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (clientId >= CELL_HTTP_MAX_CLIENTS || !s_clients[clientId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    /* Destroy all transactions belonging to this client */
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        if (s_transactions[i].in_use && s_transactions[i].client == clientId)
            s_transactions[i].in_use = 0;
    }

    s_clients[clientId].in_use = 0;
    printf("[cellHttp] DestroyClient(id=%u)\n", clientId);
    return CELL_OK;
}

s32 cellHttpCreateTransaction(CellHttpClientId clientId, const char* method,
                              const CellHttpUri* uri, CellHttpTransId* transId)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!method || !uri || !transId)
        return CELL_HTTP_ERROR_INVALID_PARAMETER;

    if (clientId >= CELL_HTTP_MAX_CLIENTS || !s_clients[clientId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        if (!s_transactions[i].in_use) {
            s_transactions[i].in_use = 1;
            s_transactions[i].client = clientId;
            s_transactions[i].status_code = 0;
            s_transactions[i].content_length = 0;
            strncpy(s_transactions[i].method, method,
                    sizeof(s_transactions[i].method) - 1);
            s_transactions[i].method[sizeof(s_transactions[i].method) - 1] = '\0';

            /* Build URL string for logging */
            snprintf(s_transactions[i].url, sizeof(s_transactions[i].url),
                     "%s://%s:%u%s",
                     uri->scheme   ? uri->scheme   : "http",
                     uri->hostname ? uri->hostname : "unknown",
                     uri->port ? uri->port : 80,
                     uri->path     ? uri->path     : "/");

            *transId = i;
            printf("[cellHttp] CreateTransaction(client=%u, %s %s) -> trans=%u\n",
                   clientId, method, s_transactions[i].url, i);
            return CELL_OK;
        }
    }

    return CELL_HTTP_ERROR_NO_MEMORY;
}

s32 cellHttpDestroyTransaction(CellHttpTransId transId)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (transId >= CELL_HTTP_MAX_TRANSACTIONS || !s_transactions[transId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    s_transactions[transId].in_use = 0;
    printf("[cellHttp] DestroyTransaction(trans=%u)\n", transId);
    return CELL_OK;
}

s32 cellHttpSendRequest(CellHttpTransId transId, const void* buf, u32 size,
                        u32* sent)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (transId >= CELL_HTTP_MAX_TRANSACTIONS || !s_transactions[transId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    printf("[cellHttp] SendRequest(trans=%u, %s %s, bodySize=%u) - STUBBED\n",
           transId, s_transactions[transId].method,
           s_transactions[transId].url, size);

    /*
     * TODO: Actually send the request via host sockets.
     * For now return connection failure so games fall back gracefully.
     */
    if (sent)
        *sent = 0;

    return CELL_HTTP_ERROR_CONNECTION_FAILED;
}

s32 cellHttpRecvResponse(CellHttpTransId transId, void* buf, u32 size,
                         u32* received)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (transId >= CELL_HTTP_MAX_TRANSACTIONS || !s_transactions[transId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    printf("[cellHttp] RecvResponse(trans=%u) - STUBBED\n", transId);

    if (received)
        *received = 0;

    return CELL_HTTP_ERROR_CONNECTION_FAILED;
}

s32 cellHttpGetResponseContentLength(CellHttpTransId transId, u64* length)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (transId >= CELL_HTTP_MAX_TRANSACTIONS || !s_transactions[transId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    if (!length)
        return CELL_HTTP_ERROR_INVALID_PARAMETER;

    *length = s_transactions[transId].content_length;
    return CELL_OK;
}

s32 cellHttpGetStatusCode(CellHttpTransId transId, s32* code)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (transId >= CELL_HTTP_MAX_TRANSACTIONS || !s_transactions[transId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    if (!code)
        return CELL_HTTP_ERROR_INVALID_PARAMETER;

    *code = s_transactions[transId].status_code;
    return CELL_OK;
}

s32 cellHttpSetResolveTimeOut(CellHttpTransId transId, u32 usec)
{
    (void)transId; (void)usec;
    printf("[cellHttp] SetResolveTimeOut(trans=%u, %u us)\n", transId, usec);
    return CELL_OK;
}

s32 cellHttpSetConnectTimeOut(CellHttpTransId transId, u32 usec)
{
    (void)transId; (void)usec;
    printf("[cellHttp] SetConnectTimeOut(trans=%u, %u us)\n", transId, usec);
    return CELL_OK;
}

s32 cellHttpSetSendTimeOut(CellHttpTransId transId, u32 usec)
{
    (void)transId; (void)usec;
    printf("[cellHttp] SetSendTimeOut(trans=%u, %u us)\n", transId, usec);
    return CELL_OK;
}

s32 cellHttpSetRecvTimeOut(CellHttpTransId transId, u32 usec)
{
    (void)transId; (void)usec;
    printf("[cellHttp] SetRecvTimeOut(trans=%u, %u us)\n", transId, usec);
    return CELL_OK;
}

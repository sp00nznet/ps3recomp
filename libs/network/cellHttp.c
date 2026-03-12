/*
 * ps3recomp - cellHttp HLE implementation
 *
 * Real HTTP client using native sockets (Winsock2 on Windows, POSIX elsewhere).
 * Resolves hostnames, connects via TCP, sends HTTP/1.1 requests, and receives
 * responses with header parsing and body streaming.
 */

#include "cellHttp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Platform socket abstraction
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET http_socket_t;
#define HTTP_INVALID_SOCKET INVALID_SOCKET
#define http_closesocket closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int http_socket_t;
#define HTTP_INVALID_SOCKET (-1)
#define http_closesocket close
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_http_initialized = 0;
#ifdef _WIN32
static int s_wsa_initialized = 0;
#endif

typedef struct {
    int                  in_use;
    u32                  resolve_timeout;
    u32                  connect_timeout;
    u32                  send_timeout;
    u32                  recv_timeout;
} HttpClientSlot;

/* Custom header key/value pair */
typedef struct {
    char name[128];
    char value[512];
} HttpCustomHeader;

/* Header receive buffer size - enough for typical HTTP response headers */
#define HTTP_HDR_BUF_SIZE 8192

typedef struct {
    int                  in_use;
    CellHttpClientId     client;
    char                 method[16];
    char                 url[1024];
    char                 hostname[256];
    char                 path[512];
    u32                  port;
    s32                  status_code;
    u64                  content_length;
    int                  content_length_known;
    u64                  request_content_length;
    int                  request_content_length_set;
    int                  aborted;

    /* Custom request headers */
    HttpCustomHeader     custom_headers[CELL_HTTP_MAX_CUSTOM_HEADERS];
    u32                  custom_header_count;

    /* Socket and recv state */
    http_socket_t        sock;
    int                  headers_parsed;
    u32                  body_received;
    int                  conn_close;       /* server sent Connection: close */
    int                  eof_reached;

    /* Buffer for accumulating response header bytes */
    char                 hdr_buf[HTTP_HDR_BUF_SIZE];
    u32                  hdr_buf_len;
    /* Leftover body bytes that arrived with the header read */
    char*                body_overflow;
    u32                  body_overflow_len;
} HttpTransSlot;

static HttpClientSlot s_clients[CELL_HTTP_MAX_CLIENTS];
static HttpTransSlot  s_transactions[CELL_HTTP_MAX_TRANSACTIONS];

/* ---------------------------------------------------------------------------
 * Helpers
 * -----------------------------------------------------------------------*/

/* Apply a timeout (in microseconds) to the transaction's socket. */
static void http_apply_timeout(http_socket_t sock, int send, u32 usec)
{
    if (sock == HTTP_INVALID_SOCKET)
        return;

#ifdef _WIN32
    /* Winsock SO_RCVTIMEO/SO_SNDTIMEO takes milliseconds as DWORD */
    DWORD ms = usec / 1000;
    if (ms == 0 && usec > 0)
        ms = 1;
    setsockopt(sock, SOL_SOCKET, send ? SO_SNDTIMEO : SO_RCVTIMEO,
               (const char*)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec  = usec / 1000000;
    tv.tv_usec = usec % 1000000;
    setsockopt(sock, SOL_SOCKET, send ? SO_SNDTIMEO : SO_RCVTIMEO,
               &tv, sizeof(tv));
#endif
}

/* Close the socket in a transaction slot if open, reset state. */
static void http_close_slot_socket(HttpTransSlot* t)
{
    if (t->sock != HTTP_INVALID_SOCKET) {
        http_closesocket(t->sock);
        t->sock = HTTP_INVALID_SOCKET;
    }
    t->headers_parsed = 0;
    t->body_received  = 0;
    t->hdr_buf_len    = 0;
    t->conn_close     = 0;
    t->eof_reached    = 0;
    if (t->body_overflow) {
        free(t->body_overflow);
        t->body_overflow = NULL;
    }
    t->body_overflow_len = 0;
}

/* Send all bytes on a socket, handling partial sends. Returns 0 on success. */
static int http_send_all(http_socket_t sock, const char* data, u32 len)
{
    u32 total = 0;
    while (total < len) {
        int n = send(sock, data + total, (int)(len - total), 0);
        if (n <= 0)
            return -1;
        total += (u32)n;
    }
    return 0;
}

/* Find "\r\n\r\n" in a buffer, return pointer to start of body or NULL. */
static const char* http_find_header_end(const char* buf, u32 len)
{
    if (len < 4)
        return NULL;
    for (u32 i = 0; i <= len - 4; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n')
            return buf + i + 4;
    }
    return NULL;
}

/* Case-insensitive strstr for header matching. */
static const char* http_strcasestr(const char* haystack, const char* needle)
{
    if (!haystack || !needle)
        return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0)
        return haystack;
    for (; *haystack; haystack++) {
        int match = 1;
        for (size_t i = 0; i < nlen; i++) {
            char a = haystack[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = 0; break; }
        }
        if (match)
            return haystack;
    }
    return NULL;
}

/* Parse the response headers that have been accumulated in hdr_buf.
 * Extracts status code, Content-Length, Connection: close.
 * Returns 0 on success, -1 on parse failure. */
static int http_parse_response_headers(HttpTransSlot* t, const char* hdr_end)
{
    u32 hdr_len = (u32)(hdr_end - t->hdr_buf);

    /* Null-terminate the header section for string ops (we have room) */
    char saved = *hdr_end;
    /* We can't write past hdr_buf if hdr_end points there, but hdr_end is
     * within hdr_buf or at the boundary. We'll work with the length instead. */

    /* Parse status line: "HTTP/1.x SSS reason\r\n" */
    int major = 0, minor = 0, status = 0;
    if (sscanf(t->hdr_buf, "HTTP/%d.%d %d", &major, &minor, &status) < 3) {
        printf("[cellHttp]   Failed to parse status line\n");
        return -1;
    }
    t->status_code = (s32)status;
    printf("[cellHttp]   Response status: %d\n", status);

    /* Temporarily null-terminate header section for searching */
    char term_buf[HTTP_HDR_BUF_SIZE];
    if (hdr_len >= HTTP_HDR_BUF_SIZE)
        hdr_len = HTTP_HDR_BUF_SIZE - 1;
    memcpy(term_buf, t->hdr_buf, hdr_len);
    term_buf[hdr_len] = '\0';

    /* Content-Length */
    t->content_length_known = 0;
    const char* cl = http_strcasestr(term_buf, "content-length:");
    if (cl) {
        cl += 15; /* skip "content-length:" */
        while (*cl == ' ') cl++;
        t->content_length = (u64)strtoull(cl, NULL, 10);
        t->content_length_known = 1;
        printf("[cellHttp]   Content-Length: %llu\n",
               (unsigned long long)t->content_length);
    }

    /* Connection: close */
    t->conn_close = 0;
    const char* conn = http_strcasestr(term_buf, "connection:");
    if (conn) {
        conn += 11;
        while (*conn == ' ') conn++;
        if (http_strcasestr(conn, "close"))
            t->conn_close = 1;
    }

    /* Store leftover body bytes */
    u32 total_in_buf = t->hdr_buf_len;
    u32 body_start   = (u32)(hdr_end - t->hdr_buf);
    u32 leftover     = total_in_buf - body_start;
    if (leftover > 0) {
        t->body_overflow = (char*)malloc(leftover);
        if (t->body_overflow) {
            memcpy(t->body_overflow, hdr_end, leftover);
            t->body_overflow_len = leftover;
        }
    }

    t->headers_parsed = 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellHttpInit(u32 poolSize)
{
    printf("[cellHttp] Init(poolSize=%u)\n", poolSize);

    if (s_http_initialized)
        return CELL_HTTP_ERROR_ALREADY_INITIALIZED;

#ifdef _WIN32
    if (!s_wsa_initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            printf("[cellHttp] WSAStartup failed\n");
            return CELL_HTTP_ERROR_UNKNOWN;
        }
        s_wsa_initialized = 1;
    }
#endif

    memset(s_clients, 0, sizeof(s_clients));
    memset(s_transactions, 0, sizeof(s_transactions));

    /* Ensure all sockets are marked invalid */
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        s_transactions[i].sock = HTTP_INVALID_SOCKET;
    }

    s_http_initialized = 1;
    return CELL_OK;
}

s32 cellHttpEnd(void)
{
    printf("[cellHttp] End()\n");

    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    /* Close any open sockets */
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        if (s_transactions[i].in_use)
            http_close_slot_socket(&s_transactions[i]);
    }

    memset(s_clients, 0, sizeof(s_clients));
    memset(s_transactions, 0, sizeof(s_transactions));

    /* Re-invalidate sockets after memset */
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        s_transactions[i].sock = HTTP_INVALID_SOCKET;
    }

    s_http_initialized = 0;

#ifdef _WIN32
    if (s_wsa_initialized) {
        WSACleanup();
        s_wsa_initialized = 0;
    }
#endif

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
        if (s_transactions[i].in_use && s_transactions[i].client == clientId) {
            http_close_slot_socket(&s_transactions[i]);
            s_transactions[i].in_use = 0;
        }
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
            HttpTransSlot* t = &s_transactions[i];
            memset(t, 0, sizeof(*t));

            t->in_use  = 1;
            t->client  = clientId;
            t->sock    = HTTP_INVALID_SOCKET;
            t->aborted = 0;
            t->custom_header_count = 0;
            t->request_content_length_set = 0;

            strncpy(t->method, method, sizeof(t->method) - 1);
            t->method[sizeof(t->method) - 1] = '\0';

            /* Store hostname, path, port separately for socket connect */
            strncpy(t->hostname,
                    uri->hostname ? uri->hostname : "unknown",
                    sizeof(t->hostname) - 1);
            t->hostname[sizeof(t->hostname) - 1] = '\0';

            strncpy(t->path,
                    uri->path ? uri->path : "/",
                    sizeof(t->path) - 1);
            t->path[sizeof(t->path) - 1] = '\0';

            t->port = uri->port ? uri->port : 80;

            /* Build URL string for logging */
            snprintf(t->url, sizeof(t->url), "%s://%s:%u%s",
                     uri->scheme   ? uri->scheme   : "http",
                     t->hostname, t->port, t->path);

            *transId = i;
            printf("[cellHttp] CreateTransaction(client=%u, %s %s) -> trans=%u\n",
                   clientId, method, t->url, i);
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

    http_close_slot_socket(&s_transactions[transId]);
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

    HttpTransSlot* t = &s_transactions[transId];

    if (t->aborted) {
        printf("[cellHttp] SendRequest(trans=%u) - transaction aborted\n", transId);
        return CELL_HTTP_ERROR_ABORTED;
    }

    printf("[cellHttp] SendRequest(trans=%u, %s %s, bodySize=%u)\n",
           transId, t->method, t->url, size);

    /* Close any previously open socket (re-send scenario) */
    http_close_slot_socket(t);

    /* ---- Resolve hostname ---- */
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", t->port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = NULL;
    int gai = getaddrinfo(t->hostname, port_str, &hints, &result);
    if (gai != 0 || !result) {
        printf("[cellHttp]   getaddrinfo failed for '%s': %d\n", t->hostname, gai);
        if (result) freeaddrinfo(result);
        if (sent) *sent = 0;
        return CELL_HTTP_ERROR_CONNECTION_FAILED;
    }

    /* ---- Create socket and connect ---- */
    http_socket_t sock = socket(result->ai_family, result->ai_socktype,
                                result->ai_protocol);
    if (sock == HTTP_INVALID_SOCKET) {
        printf("[cellHttp]   socket() failed\n");
        freeaddrinfo(result);
        if (sent) *sent = 0;
        return CELL_HTTP_ERROR_CONNECTION_FAILED;
    }

    /* Apply timeouts from the owning client */
    HttpClientSlot* c = &s_clients[t->client];
    http_apply_timeout(sock, 1, c->send_timeout);
    http_apply_timeout(sock, 0, c->recv_timeout);

    if (connect(sock, result->ai_addr, (int)result->ai_addrlen) != 0) {
        printf("[cellHttp]   connect() failed to %s:%u\n", t->hostname, t->port);
        http_closesocket(sock);
        freeaddrinfo(result);
        if (sent) *sent = 0;
        return CELL_HTTP_ERROR_CONNECTION_FAILED;
    }

    freeaddrinfo(result);
    printf("[cellHttp]   Connected to %s:%u\n", t->hostname, t->port);

    /* ---- Format HTTP request ---- */
    char req_buf[4096];
    int req_len = snprintf(req_buf, sizeof(req_buf),
                           "%s %s HTTP/1.1\r\n"
                           "Host: %s\r\n",
                           t->method, t->path, t->hostname);

    /* Content-Length header if body provided or explicitly set */
    if (size > 0) {
        req_len += snprintf(req_buf + req_len, sizeof(req_buf) - (u32)req_len,
                            "Content-Length: %u\r\n", size);
    } else if (t->request_content_length_set) {
        req_len += snprintf(req_buf + req_len, sizeof(req_buf) - (u32)req_len,
                            "Content-Length: %llu\r\n",
                            (unsigned long long)t->request_content_length);
    }

    /* Append custom headers */
    for (u32 i = 0; i < t->custom_header_count; i++) {
        req_len += snprintf(req_buf + req_len, sizeof(req_buf) - (u32)req_len,
                            "%s: %s\r\n",
                            t->custom_headers[i].name,
                            t->custom_headers[i].value);
    }

    /* End of headers */
    req_len += snprintf(req_buf + req_len, sizeof(req_buf) - (u32)req_len,
                        "\r\n");

    /* ---- Send header ---- */
    if (http_send_all(sock, req_buf, (u32)req_len) != 0) {
        printf("[cellHttp]   Failed to send request headers\n");
        http_closesocket(sock);
        if (sent) *sent = 0;
        return CELL_HTTP_ERROR_SEND_FAILED;
    }

    /* ---- Send body if provided ---- */
    if (buf && size > 0) {
        if (http_send_all(sock, (const char*)buf, size) != 0) {
            printf("[cellHttp]   Failed to send request body\n");
            http_closesocket(sock);
            if (sent) *sent = 0;
            return CELL_HTTP_ERROR_SEND_FAILED;
        }
    }

    printf("[cellHttp]   Request sent (%d header bytes + %u body bytes)\n",
           req_len, size);

    /* Store socket for recv */
    t->sock           = sock;
    t->headers_parsed = 0;
    t->body_received  = 0;
    t->hdr_buf_len    = 0;
    t->eof_reached    = 0;
    t->body_overflow  = NULL;
    t->body_overflow_len = 0;

    if (sent)
        *sent = size;

    return CELL_OK;
}

s32 cellHttpRecvResponse(CellHttpTransId transId, void* buf, u32 size,
                         u32* received)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (transId >= CELL_HTTP_MAX_TRANSACTIONS || !s_transactions[transId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    HttpTransSlot* t = &s_transactions[transId];

    if (t->aborted) {
        printf("[cellHttp] RecvResponse(trans=%u) - transaction aborted\n", transId);
        if (received) *received = 0;
        return CELL_HTTP_ERROR_ABORTED;
    }

    if (t->sock == HTTP_INVALID_SOCKET) {
        printf("[cellHttp] RecvResponse(trans=%u) - no socket (send first)\n",
               transId);
        if (received) *received = 0;
        return CELL_HTTP_ERROR_CONNECTION_FAILED;
    }

    /* ---- Parse response headers on first call ---- */
    if (!t->headers_parsed) {
        printf("[cellHttp] RecvResponse(trans=%u) - reading response headers\n",
               transId);

        /* Read until we find \r\n\r\n */
        while (t->hdr_buf_len < HTTP_HDR_BUF_SIZE - 1) {
            int n = recv(t->sock, t->hdr_buf + t->hdr_buf_len,
                         (int)(HTTP_HDR_BUF_SIZE - 1 - t->hdr_buf_len), 0);
            if (n <= 0) {
                printf("[cellHttp]   recv() failed during header read (n=%d)\n", n);
                http_close_slot_socket(t);
                if (received) *received = 0;
                return (n == 0) ? CELL_HTTP_ERROR_RECV_FAILED
                                : CELL_HTTP_ERROR_RECV_FAILED;
            }
            t->hdr_buf_len += (u32)n;

            const char* hdr_end = http_find_header_end(t->hdr_buf,
                                                       t->hdr_buf_len);
            if (hdr_end) {
                if (http_parse_response_headers(t, hdr_end) != 0) {
                    http_close_slot_socket(t);
                    if (received) *received = 0;
                    return CELL_HTTP_ERROR_RECV_FAILED;
                }
                break;
            }
        }

        if (!t->headers_parsed) {
            printf("[cellHttp]   Header buffer overflow, no \\r\\n\\r\\n found\n");
            http_close_slot_socket(t);
            if (received) *received = 0;
            return CELL_HTTP_ERROR_RECV_FAILED;
        }
    }

    /* ---- Return body data ---- */
    if (!buf || size == 0) {
        if (received) *received = 0;
        return CELL_OK;
    }

    /* Check if we've already received all expected body bytes */
    if (t->content_length_known && t->body_received >= t->content_length) {
        if (received) *received = 0;
        return CELL_OK;
    }

    if (t->eof_reached) {
        if (received) *received = 0;
        return CELL_OK;
    }

    u32 filled = 0;

    /* First drain any leftover body bytes from the header read */
    if (t->body_overflow && t->body_overflow_len > 0) {
        u32 copy = t->body_overflow_len;
        if (copy > size)
            copy = size;
        memcpy(buf, t->body_overflow, copy);
        filled += copy;
        t->body_received += copy;

        if (copy < t->body_overflow_len) {
            /* Still have overflow remaining */
            u32 remain = t->body_overflow_len - copy;
            memmove(t->body_overflow, t->body_overflow + copy, remain);
            t->body_overflow_len = remain;
        } else {
            free(t->body_overflow);
            t->body_overflow = NULL;
            t->body_overflow_len = 0;
        }
    }

    /* Read more from socket if caller wants more */
    while (filled < size) {
        /* If content-length known, don't read past it */
        u32 want = size - filled;
        if (t->content_length_known) {
            u64 remaining = t->content_length - t->body_received;
            if (remaining == 0)
                break;
            if (want > remaining)
                want = (u32)remaining;
        }

        if (want == 0)
            break;

        int n = recv(t->sock, (char*)buf + filled, (int)want, 0);
        if (n < 0) {
            /* Error - if we already have some data, return it */
            if (filled > 0)
                break;
            printf("[cellHttp]   recv() error during body read\n");
            if (received) *received = 0;
            return CELL_HTTP_ERROR_RECV_FAILED;
        }
        if (n == 0) {
            /* Connection closed by peer */
            t->eof_reached = 1;
            if (!t->content_length_known) {
                /* For Connection: close responses, EOF is normal end */
                t->content_length = t->body_received;
                t->content_length_known = 1;
            }
            break;
        }

        filled += (u32)n;
        t->body_received += (u32)n;
    }

    if (received)
        *received = filled;

    return CELL_OK;
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
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (transId >= CELL_HTTP_MAX_TRANSACTIONS || !s_transactions[transId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    HttpTransSlot* t = &s_transactions[transId];
    HttpClientSlot* c = &s_clients[t->client];
    c->resolve_timeout = usec;
    printf("[cellHttp] SetResolveTimeOut(trans=%u, %u us)\n", transId, usec);
    return CELL_OK;
}

s32 cellHttpSetConnectTimeOut(CellHttpTransId transId, u32 usec)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (transId >= CELL_HTTP_MAX_TRANSACTIONS || !s_transactions[transId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    HttpTransSlot* t = &s_transactions[transId];
    HttpClientSlot* c = &s_clients[t->client];
    c->connect_timeout = usec;
    printf("[cellHttp] SetConnectTimeOut(trans=%u, %u us)\n", transId, usec);
    return CELL_OK;
}

s32 cellHttpSetSendTimeOut(CellHttpTransId transId, u32 usec)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (transId >= CELL_HTTP_MAX_TRANSACTIONS || !s_transactions[transId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    HttpTransSlot* t = &s_transactions[transId];
    HttpClientSlot* c = &s_clients[t->client];
    c->send_timeout = usec;

    /* Apply immediately if socket is already open */
    if (t->sock != HTTP_INVALID_SOCKET)
        http_apply_timeout(t->sock, 1, usec);

    printf("[cellHttp] SetSendTimeOut(trans=%u, %u us)\n", transId, usec);
    return CELL_OK;
}

s32 cellHttpSetRecvTimeOut(CellHttpTransId transId, u32 usec)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (transId >= CELL_HTTP_MAX_TRANSACTIONS || !s_transactions[transId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    HttpTransSlot* t = &s_transactions[transId];
    HttpClientSlot* c = &s_clients[t->client];
    c->recv_timeout = usec;

    /* Apply immediately if socket is already open */
    if (t->sock != HTTP_INVALID_SOCKET)
        http_apply_timeout(t->sock, 0, usec);

    printf("[cellHttp] SetRecvTimeOut(trans=%u, %u us)\n", transId, usec);
    return CELL_OK;
}

s32 cellHttpSetRequestContentLength(CellHttpTransId transId, u64 length)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (transId >= CELL_HTTP_MAX_TRANSACTIONS || !s_transactions[transId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    s_transactions[transId].request_content_length     = length;
    s_transactions[transId].request_content_length_set = 1;
    printf("[cellHttp] SetRequestContentLength(trans=%u, %llu)\n",
           transId, (unsigned long long)length);
    return CELL_OK;
}

s32 cellHttpAddRequestHeader(CellHttpTransId transId, const char* name,
                             const char* value)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (transId >= CELL_HTTP_MAX_TRANSACTIONS || !s_transactions[transId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    if (!name || !value)
        return CELL_HTTP_ERROR_INVALID_PARAMETER;

    HttpTransSlot* t = &s_transactions[transId];
    if (t->custom_header_count >= CELL_HTTP_MAX_CUSTOM_HEADERS) {
        printf("[cellHttp] AddRequestHeader - too many custom headers\n");
        return CELL_HTTP_ERROR_NO_BUFFER;
    }

    HttpCustomHeader* h = &t->custom_headers[t->custom_header_count];
    strncpy(h->name, name, sizeof(h->name) - 1);
    h->name[sizeof(h->name) - 1] = '\0';
    strncpy(h->value, value, sizeof(h->value) - 1);
    h->value[sizeof(h->value) - 1] = '\0';
    t->custom_header_count++;

    printf("[cellHttp] AddRequestHeader(trans=%u, '%s: %s')\n",
           transId, name, value);
    return CELL_OK;
}

s32 cellHttpAbortTransaction(CellHttpTransId transId)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (transId >= CELL_HTTP_MAX_TRANSACTIONS || !s_transactions[transId].in_use)
        return CELL_HTTP_ERROR_NOT_FOUND;

    HttpTransSlot* t = &s_transactions[transId];
    t->aborted = 1;
    http_close_slot_socket(t);
    printf("[cellHttp] AbortTransaction(trans=%u)\n", transId);
    return CELL_OK;
}

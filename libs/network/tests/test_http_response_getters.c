/*
 * test_http_response_getters - cellHttpResponseGetStatusCode,
 * cellHttpResponseGetContentLength, cellHttpResponseGetAllHeaders, and a
 * body larger than the header read through cellHttpRecvResponse.
 *
 * A loopback listener answers each request. The getters are called after
 * cellHttpSendRequest and before the first cellHttpRecvResponse, as titles
 * do; the rest of the body is only written after the headers were read, so
 * cellHttpRecvResponse has to recv() it straight into the guest buffer.
 * The getters are declared weak so a build without them still runs and
 * reports them missing. POSIX sockets; on Windows it reports SKIP.
 *
 *   cc -std=gnu17 -I include -o test_http_response_getters libs/network/tests/test_http_response_getters.c
 * Driven by tools/test_http_response_getters.py.
 */
#ifdef _WIN32
#include <stdio.h>
int main(void) { puts("SKIP: POSIX sockets only"); return 0; }
#else
#include "../cellHttp.h"
s32 cellHttpResponseGetStatusCode(CellHttpTransId, s32*) __attribute__((weak));
s32 cellHttpResponseGetContentLength(CellHttpTransId, u64*) __attribute__((weak));
s32 cellHttpResponseGetAllHeaders(CellHttpTransId, void*, u32*, void*, u32, u32*) __attribute__((weak));
#include "../cellHttp.c"
#include <arpa/inet.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_linelock_lock(uint32_t a) { (void)a; }
void spu_linelock_release(uint32_t a) { (void)a; }

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

enum { MEM = 1 << 20, URI = 0x200, CLIENT = 0x300, TRANS = 0x304, U32OUT = 0x308, U64OUT = 0x310,
       ITEMS = 0x318, REQUIRED = 0x31C, HEADERS = 0x320, POOL = 0x1000, BODY = 0x10000 };
#define BODY_LEN 3000

static int lfd;
static u16 port;
static CellHttpClientId client;

/* Open a transaction and send it; return the server side of the connection. */
static int send_request(CellHttpTransId* trans)
{
    vm_write32(URI + 0, 0x100); vm_write32(URI + 4, 0x110); vm_write32(URI + 8, 0);
    vm_write32(URI + 12, 0);    vm_write32(URI + 16, 0x140); vm_write32(URI + 20, port);
    vm_write32(URI + 24, 0);
    if (cellHttpCreateTransaction((CellHttpTransId*)(uintptr_t)TRANS, client,
                                  (const char*)(uintptr_t)0x180, (const CellHttpUri*)(uintptr_t)URI) != CELL_OK)
        return -1;
    *trans = vm_read32(TRANS);
    if (cellHttpSendRequest(*trans, NULL, 0, (u32*)(uintptr_t)U32OUT) != CELL_OK) return -1;
    int cfd = accept(lfd, NULL, NULL);
    char req[2048]; size_t n = 0;
    while (cfd >= 0 && n < sizeof req - 1) {
        ssize_t r = recv(cfd, req + n, sizeof req - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r; req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    return cfd;
}

static void send_str(int fd, const char* s) { send(fd, s, strlen(s), 0); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = calloc(1, MEM);
    ppu_vm_size = MEM;
    strcpy((char*)vm_base + 0x100, "http");
    strcpy((char*)vm_base + 0x110, "127.0.0.1");
    strcpy((char*)vm_base + 0x140, "/list.jsp");
    strcpy((char*)vm_base + 0x180, "GET");

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (lfd < 0 || bind(lfd, (struct sockaddr*)&a, sizeof a) != 0 || listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr*)&a, &al) != 0) {
        puts("SKIP: no loopback listener");
        return 0;
    }
    port = ntohs(a.sin_port);
    if (cellHttpInit(NULL, 0) != CELL_OK ||
        cellHttpCreateClient((CellHttpClientId*)(uintptr_t)CLIENT) != CELL_OK) { puts("FAIL: init"); return 1; }
    client = *(CellHttpClientId*)(vm_base + CLIENT);   /* written host-endian */

    char body[BODY_LEN];
    for (int i = 0; i < BODY_LEN; i++) body[i] = (char)('a' + i % 26);

    /* 1: three headers, Content-Length, a body that outgrows the header read. */
    CellHttpTransId t1;
    int cfd = send_request(&t1);
    CHECK(cfd >= 0, "request 1 sent and accepted");
    char hdr[256];
    snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nX-Region: us\r\n"
                              "Content-Length: %d\r\n\r\n", BODY_LEN);
    send_str(cfd, hdr);
    send(cfd, body, 100, 0);   /* the rest is sent after the headers were read */

    CHECK(cellHttpResponseGetStatusCode != NULL, "cellHttpResponseGetStatusCode exists");
    CHECK(cellHttpResponseGetContentLength != NULL, "cellHttpResponseGetContentLength exists");
    CHECK(cellHttpResponseGetAllHeaders != NULL, "cellHttpResponseGetAllHeaders exists");
    if (cellHttpResponseGetStatusCode) {
        s32 r = cellHttpResponseGetStatusCode(t1, (s32*)(uintptr_t)U32OUT);
        CHECK(r == CELL_OK && vm_read32(U32OUT) == 200, "status code %u (r=0x%08X)", vm_read32(U32OUT), (u32)r);
    }
    if (cellHttpResponseGetContentLength) {
        s32 r = cellHttpResponseGetContentLength(t1, (u64*)(uintptr_t)U64OUT);
        u64 len = ((u64)vm_read32(U64OUT) << 32) | vm_read32(U64OUT + 4);
        CHECK(r == CELL_OK && len == BODY_LEN, "content length %llu (r=0x%08X)", (unsigned long long)len, (u32)r);
    }
    if (cellHttpResponseGetAllHeaders) {
        s32 r = cellHttpResponseGetAllHeaders(t1, NULL, (u32*)(uintptr_t)ITEMS, NULL, 0, (u32*)(uintptr_t)REQUIRED);
        u32 need = vm_read32(REQUIRED);
        CHECK(r == CELL_OK && need > 0 && vm_read32(ITEMS) == 0, "size query: %u bytes required", need);
        r = cellHttpResponseGetAllHeaders(t1, (void*)(uintptr_t)HEADERS, (u32*)(uintptr_t)ITEMS,
                                          (void*)(uintptr_t)POOL, need, (u32*)(uintptr_t)REQUIRED);
        u32 items = vm_read32(ITEMS), arr = vm_read32(HEADERS);
        CHECK(r == CELL_OK && items == 3 && arr == POOL, "%u headers at 0x%X", items, arr);
        static const char* want[3][2] = {{"Content-Type", "text/xml"}, {"X-Region", "us"},
                                         {"Content-Length", "3000"}};
        for (u32 k = 0; k < 3 && k < items; k++) {
            const char* nm = (const char*)vm_base + vm_read32(arr + k * 8);
            const char* vl = (const char*)vm_base + vm_read32(arr + k * 8 + 4);
            CHECK(strcmp(nm, want[k][0]) == 0 && strcmp(vl, want[k][1]) == 0, "header %u: \"%s: %s\"", k, nm, vl);
        }
    }
    /* Headers read (a no-op when a getter already read them), then the rest
     * of the body: it can only arrive through recv() into the guest buffer. */
    cellHttpRecvResponse(t1, NULL, 0, NULL);
    send(cfd, body + 100, BODY_LEN - 100, 0);
    u32 got = 0;
    for (int i = 0; i < 16 && got < BODY_LEN; i++) {
        s32 r = cellHttpRecvResponse(t1, (void*)(uintptr_t)(BODY + got), BODY_LEN - got, (u32*)(uintptr_t)U32OUT);
        u32 n = vm_read32(U32OUT);
        if (r != CELL_OK || n == 0) break;
        got += n;
    }
    CHECK(got == BODY_LEN && memcmp(vm_base + BODY, body, BODY_LEN) == 0,
          "body: %u of %d bytes in the guest buffer", got, BODY_LEN);
    close(cfd);
    cellHttpDestroyTransaction(t1);

    /* 2: no Content-Length (the body runs to the end of the connection). */
    if (cellHttpResponseGetContentLength) {
        CellHttpTransId t2;
        cfd = send_request(&t2);
        send_str(cfd, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nshort");
        close(cfd);
        s32 r = cellHttpResponseGetContentLength(t2, (u64*)(uintptr_t)U64OUT);
        CHECK((u32)r == 0x80710017u, "no Content-Length: 0x%08X (want 0x80710017)", (u32)r);
        cellHttpDestroyTransaction(t2);
    }

    cellHttpDestroyClient(client);
    cellHttpEnd();
    close(lfd);
    free(vm_base);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
#endif

/*
 * test_http_uri_path - cellHttpCreateTransaction takes the request path from
 * CellHttpUri's path field.
 *
 * The guest CellHttpUri is { scheme, hostname, username, password, path,
 * port, reserved } with 4-byte EAs: the path is at +16, the username at +8.
 * A loopback listener receives the request cellHttpSendRequest writes and
 * the test checks its request line carries the path the title asked for.
 * POSIX sockets; on Windows it reports SKIP.
 *
 *   cc -std=gnu17 -I include -o test_http_uri_path libs/network/tests/test_http_uri_path.c
 * Driven by tools/test_http_uri_path.py.
 */
#ifdef _WIN32
#include <stdio.h>
int main(void) { puts("SKIP: POSIX sockets only"); return 0; }
#else
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

static void put_str(u32 ea, const char* s) { strcpy((char*)vm_base + ea, s); }

/* Send one request for a URI whose username field is `user_ea` and return
 * the request line the listener saw (empty on failure). */
static void request_line(int lfd, u16 port, u32 user_ea, char* line, size_t cap)
{
    line[0] = 0;
    const u32 URI = 0x200, CLIENT = 0x300, TRANS = 0x304, SENT = 0x308;
    vm_write32(URI + 0,  0x100);       /* scheme   */
    vm_write32(URI + 4,  0x110);       /* hostname */
    vm_write32(URI + 8,  user_ea);     /* username */
    vm_write32(URI + 12, 0);           /* password */
    vm_write32(URI + 16, 0x140);       /* path     */
    vm_write32(URI + 20, port);        /* port     */
    vm_write32(URI + 24, 0);           /* reserved */

    if (cellHttpCreateClient((CellHttpClientId*)(uintptr_t)CLIENT) != CELL_OK) return;
    CellHttpClientId client = *(CellHttpClientId*)(vm_base + CLIENT);   /* written host-endian */
    if (cellHttpCreateTransaction((CellHttpTransId*)(uintptr_t)TRANS, client,
                                  (const char*)(uintptr_t)0x180, (const CellHttpUri*)(uintptr_t)URI) != CELL_OK)
        return;
    CellHttpTransId trans = vm_read32(TRANS);
    if (cellHttpSendRequest(trans, NULL, 0, (u32*)(uintptr_t)SENT) != CELL_OK) return;

    int cfd = accept(lfd, NULL, NULL);   /* the connect completed from the backlog */
    if (cfd < 0) return;
    char buf[2048]; size_t n = 0;
    while (n < sizeof buf - 1) {
        ssize_t r = recv(cfd, buf + n, sizeof buf - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r; buf[n] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[n] = 0;
    close(cfd);
    char* eol = strstr(buf, "\r\n");
    if (eol) *eol = 0;
    snprintf(line, cap, "%s", buf);
    cellHttpDestroyTransaction(trans);
    cellHttpDestroyClient(client);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = calloc(1, 65536);
    ppu_vm_size = 65536;
    put_str(0x100, "http");
    put_str(0x110, "127.0.0.1");
    put_str(0x120, "player");
    put_str(0x140, "/stats/region_list.jsp");
    put_str(0x180, "GET");

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    socklen_t al = sizeof a;
    if (lfd < 0 || bind(lfd, (struct sockaddr*)&a, sizeof a) != 0 || listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr*)&a, &al) != 0) {
        puts("SKIP: no loopback listener");
        return 0;
    }
    u16 port = ntohs(a.sin_port);

    if (cellHttpInit(NULL, 0) != CELL_OK) { puts("FAIL: cellHttpInit"); return 1; }
    char line[512];
    request_line(lfd, port, 0, line, sizeof line);
    CHECK(strcmp(line, "GET /stats/region_list.jsp HTTP/1.1") == 0,
          "no username: request line \"%s\"", line);
    request_line(lfd, port, 0x120, line, sizeof line);
    CHECK(strcmp(line, "GET /stats/region_list.jsp HTTP/1.1") == 0,
          "username set: request line \"%s\"", line);
    cellHttpEnd();
    close(lfd);
    free(vm_base);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
#endif

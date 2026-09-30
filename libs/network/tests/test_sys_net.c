/*
 * test_sys_net - libnet's host-socket path, end to end over loopback.
 *
 * Self-contained: it compiles sysNet.c into this file, gives it a 1 MB guest
 * arena, and calls every handler through the NID it registered -- so a wrong
 * export name fails here, not silently in a title (the old sysNet.c hashed
 * its C function names and no guest call ever reached it).
 *
 *   clang-cl /I include /Fe:test_sys_net.exe libs/network/tests/test_sys_net.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_sys_net libs/network/tests/test_sys_net.c
 */

#include "../sysNet.c"

#include <assert.h>

/* ppu_memory.h hooks the runtime supplies; inert here. */
uint8_t* vm_base;
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t addr) { (void)addr; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width) { (void)addr; (void)val; (void)width; }
static uint16_t s_p2p_port = 36658;
uint16_t np_psnr_p2p_port(void) { return s_p2p_port; }

/* np_psnr's side of NAT traversal (tested for real in test_np_nat.c): here,
 * only that sysNet registers the P2P datagram socket and drops what the
 * filter claims. */
static np_psnr_p2p_send_fn s_sender;
static np_psnr_stream_sink_fn s_sink;
void np_psnr_set_p2p_sender(np_psnr_p2p_send_fn fn) { s_sender = fn; }
void np_psnr_set_stream_sink(np_psnr_stream_sink_fn fn) { s_sink = fn; }
int  np_psnr_p2p_filter(void* buf, uint32_t* len, uint8_t ip[4], uint16_t* port)
{
    (void)ip; (void)port;
    return *len >= 4 && !memcmp(buf, "PSNR", 4);
}
int     np_psnr_p2p_route(const void* b, uint32_t l, const uint8_t ip[4], uint16_t p) { (void)b; (void)l; (void)ip; (void)p; return 0; }
int64_t np_psnr_stream_connect(const uint8_t ip[4], uint16_t p) { (void)ip; (void)p; return -2; }

static struct { uint32_t nid; void (*fn)(ppu_context*); } s_reg[64];
static int s_nreg;
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    (void)name;
    s_reg[s_nreg].nid = nid;
    s_reg[s_nreg].fn = fn;
    s_nreg++;
}

/* Hands a relayed stream to sysNet's sink from another thread, the way the
 * pump thread does while the title's thread sits in a blocking accept. */
static struct { int64_t fd; uint8_t ip[4]; uint16_t port; } s_late;
#ifdef _WIN32
static DWORD WINAPI late_sink(LPVOID u) { (void)u; Sleep(200); s_sink(s_late.fd, s_late.ip, s_late.port); return 0; }
static void start_late_sink(void) { CloseHandle(CreateThread(NULL, 0, late_sink, NULL, 0, NULL)); }
#else
#include <pthread.h>
static void* late_sink(void* u) { (void)u; usleep(200 * 1000); s_sink(s_late.fd, s_late.ip, s_late.port); return NULL; }
static void start_late_sink(void) { pthread_t t; pthread_create(&t, NULL, late_sink, NULL); pthread_detach(t); }
#endif

static unsigned int bump_alloc(unsigned int size, unsigned int align)
{
    static unsigned int top = 0x80000;
    top = (top + align - 1) & ~(align - 1);
    unsigned int a = top;
    top += size;
    return a;
}

/* Call an import by name, the way a lifted title does: by its NID. */
static int64_t call(const char* name, uint64_t a3, uint64_t a4, uint64_t a5,
                    uint64_t a6, uint64_t a7, uint64_t a8)
{
    uint32_t nid = ps3_compute_nid(name);
    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = a3; ctx.gpr[4] = a4; ctx.gpr[5] = a5;
    ctx.gpr[6] = a6; ctx.gpr[7] = a7; ctx.gpr[8] = a8;
    for (int i = 0; i < s_nreg; i++)
        if (s_reg[i].nid == nid) { s_reg[i].fn(&ctx); return (int64_t)ctx.gpr[3]; }
    fprintf(stderr, "no handler for %s (0x%08X)\n", name, nid);
    assert(0);
    return 0;
}
#define C1(n, a)             (int32_t)call(n, a, 0, 0, 0, 0, 0)
#define C3(n, a, b, c)       (int32_t)call(n, a, b, c, 0, 0, 0)
#define C5(n, a, b, c, d, e) (int32_t)call(n, a, b, c, d, e, 0)

/* Guest scratch addresses */
enum { ADDR_A = 0x1000, ADDR_B = 0x1100, LEN = 0x1200, OPT = 0x1300,
       BUF = 0x2000, POLLFD = 0x3000, RSET = 0x4000, TV = 0x4100, STR = 0x5000 };

static uint32_t errno_cell(void) { return vm_read32((uint32_t)call("_sys_net_errno_loc", 0, 0, 0, 0, 0, 0)); }

static void put_sockaddr(uint32_t ea, uint32_t ip_be, uint16_t port)
{
    memset(vm_base + ea, 0, 16);
    vm_write8(ea, 16);
    vm_write8(ea + 1, SYS_NET_AF_INET);
    vm_write16(ea + 2, port);
    vm_write32(ea + 4, ip_be);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 1u << 20);
    ps3_net_host_register(bump_alloc);
    assert(s_reg[0].nid == 0x9C056962u);   /* "socket", as nid_database.py has it */

    const uint32_t LOOP = 0x7F000001u;

    /* UDP: bind A to an ephemeral port, send from B, poll, receive. */
    int32_t a = C3("socket", SYS_NET_AF_INET, SYS_NET_SOCK_DGRAM, 0);
    int32_t b = C3("socket", SYS_NET_AF_INET, SYS_NET_SOCK_DGRAM, 0);
    assert(a > 0 && b > 0 && a != b);
    put_sockaddr(ADDR_A, LOOP, 0);
    assert(C3("bind", a, ADDR_A, 16) == 0);
    vm_write32(LEN, 16);
    assert(C3("getsockname", a, ADDR_A, LEN) == 0);
    uint16_t port_a = vm_read16(ADDR_A + 2);
    assert(port_a != 0 && vm_read8(ADDR_A + 1) == SYS_NET_AF_INET && vm_read32(LEN) == 16);

    memcpy(vm_base + BUF, "hello", 5);
    assert(call("sendto", b, BUF, 5, 0, ADDR_A, 16) == 5);

    vm_write32(POLLFD, (uint32_t)a);
    vm_write16(POLLFD + 4, SYS_NET_POLLIN);
    vm_write16(POLLFD + 6, 0xFFFF);
    assert(C3("socketpoll", POLLFD, 1, 1000) == 1);
    assert(vm_read16(POLLFD + 6) & SYS_NET_POLLIN);

    memset(vm_base + BUF, 0, 16);
    assert(call("recvfrom", a, BUF, 64, 0, ADDR_B, LEN) == 5);
    assert(memcmp(vm_base + BUF, "hello", 5) == 0);
    assert(vm_read32(ADDR_B + 4) == LOOP);

    /* Nothing queued: SO_NBIO and MSG_DONTWAIT both give -1 / EWOULDBLOCK (35). */
    assert((uint32_t)call("recvfrom", a, BUF, 64, SYS_NET_MSG_DONTWAIT, 0, 0) == 0x80010223u);
    assert(errno_cell() == SYS_NET_EWOULDBLOCK);
    vm_write32(OPT, 1);
    assert(C5("setsockopt", a, SYS_NET_SOL_SOCKET, SYS_NET_SO_NBIO, OPT, 4) == 0);
    assert((uint32_t)call("recvfrom", a, BUF, 64, 0, 0, 0) == 0x80010223u);
    assert(errno_cell() == SYS_NET_EWOULDBLOCK);
    vm_write32(LEN, 4);
    assert(C5("getsockopt", a, SYS_NET_SOL_SOCKET, SYS_NET_SO_NBIO, OPT, LEN) == 0);
    assert(vm_read32(OPT) == 1);

    /* TCP: listen, connect, accept, select for readability, round-trip. */
    int32_t l = C3("socket", SYS_NET_AF_INET, SYS_NET_SOCK_STREAM, 0);
    put_sockaddr(ADDR_A, LOOP, 0);
    assert(C3("bind", l, ADDR_A, 16) == 0);
    assert(C3("listen", l, 4, 0) == 0);
    assert(C3("getsockname", l, ADDR_A, LEN) == 0);
    int32_t c = C3("socket", SYS_NET_AF_INET, SYS_NET_SOCK_STREAM, 0);
    assert(C3("connect", c, ADDR_A, 16) == 0);
    int32_t srv = C3("accept", l, ADDR_B, LEN);
    assert(srv > 0 && vm_read32(ADDR_B + 4) == LOOP);

    memcpy(vm_base + BUF, "ping", 4);
    assert(call("send", c, BUF, 4, 0, 0, 0) == 4);
    memset(vm_base + RSET, 0, 128);
    vm_write32(RSET + (uint32_t)srv / 32 * 4, 1u << (srv % 32));
    vm_write64(TV, 1);       /* 1 s */
    vm_write64(TV + 8, 0);
    assert(C5("socketselect", srv + 1, RSET, 0, 0, TV) == 1);
    assert(vm_read32(RSET + (uint32_t)srv / 32 * 4) & (1u << (srv % 32)));
    memset(vm_base + BUF, 0, 16);
    assert(call("recv", srv, BUF, 16, 0, 0, 0) == 4);
    assert(memcmp(vm_base + BUF, "ping", 4) == 0);

    /* Address helpers and DNS. */
    strcpy((char*)vm_base + STR, "1.2.3.4");
    assert((uint32_t)call("inet_addr", STR, 0, 0, 0, 0, 0) == 0x01020304u);
    uint32_t s = (uint32_t)call("inet_ntoa", LOOP, 0, 0, 0, 0, 0);
    assert(strcmp((const char*)vm_base + s, "127.0.0.1") == 0);
    strcpy((char*)vm_base + STR, "localhost");
    uint32_t h = (uint32_t)call("gethostbyname", STR, 0, 0, 0, 0, 0);
    assert(h && vm_read32(h + 12) == 4);
    assert(vm_read32(vm_read32(vm_read32(h + 16))) == LOOP);   /* *h_addr_list[0] */

    /* P2P: the title binds its own IP and port 3658 with vport 1000; the host
     * socket lands on this instance's P2P port on every interface, so a peer
     * on loopback reaches it, and received addresses carry the vport back. */
    int32_t p2p = C3("socket", SYS_NET_AF_INET, 6 /* SOCK_DGRAM_P2P */, 0);
    put_sockaddr(ADDR_A, 0x0A000001u /* 10.0.0.1: not ours */, 3658);
    vm_write16(ADDR_A + 8, 1000);
    assert(C3("bind", p2p, ADDR_A, 16) == 0);
    vm_write32(LEN, 16);
    assert(C3("getsockname", p2p, ADDR_B, LEN) == 0);
    assert(vm_read16(ADDR_B + 2) == 36658 && vm_read32(ADDR_B + 4) == 0);
    put_sockaddr(ADDR_A, LOOP, 36658);
    memcpy(vm_base + BUF, "p2p", 3);
    assert(call("sendto", p2p, BUF, 3, 0, ADDR_A, 16) == 3);   /* to itself */
    assert(C3("socketpoll", POLLFD, 0, 0) == 0);
    vm_write32(POLLFD, (uint32_t)p2p);
    vm_write16(POLLFD + 4, SYS_NET_POLLIN);
    assert(C3("socketpoll", POLLFD, 1, 1000) == 1);
    assert(call("recvfrom", p2p, BUF, 64, 0, ADDR_B, LEN) == 3);
    assert(vm_read16(ADDR_B + 8) == 1000);

    /* The bound P2P datagram socket is np_psnr's to send probes and punches
     * from, and psnr's packets arriving on it never reach the title: a
     * control packet followed by the title's own data reads as the data. */
    {
        const uint8_t lo[4] = { 127, 0, 0, 1 };
        assert(s_sender);
        assert(s_sender("PSNR\x02zzzz", 9, lo, 36658) == 9);
        assert(s_sender("game", 4, lo, 36658) == 4);
        vm_write32(POLLFD, (uint32_t)p2p);
        vm_write16(POLLFD + 4, SYS_NET_POLLIN);
        assert(C3("socketpoll", POLLFD, 1, 1000) == 1);
        vm_write32(LEN, 16);
        assert(call("recvfrom", p2p, BUF, 64, 0, ADDR_B, LEN) == 4);
        assert(memcmp(vm_base + BUF, "game", 4) == 0);
    }
    assert(C1("socketclose", p2p) == 0);
    assert(!s_sender);   /* closing it unregisters */

    /* P2P streams: an instance's listener and the socket it connects out
     * with share its P2P port, so the peer accepts a connection whose source
     * port is the P2P port signaling reported -- titles check it. */
    int32_t l1 = C3("socket", SYS_NET_AF_INET, 10 /* SOCK_STREAM_P2P */, 0);
    put_sockaddr(ADDR_A, LOOP, 4099);
    assert(C3("bind", l1, ADDR_A, 16) == 0 && C1("listen", l1) == 0);
    s_p2p_port = 36659;
    int32_t l2 = C3("socket", SYS_NET_AF_INET, 10, 0);
    int32_t out = C3("socket", SYS_NET_AF_INET, 10, 0);
    assert(C3("bind", l2, ADDR_A, 16) == 0 && C1("listen", l2) == 0);
    assert(C3("bind", out, ADDR_A, 16) == 0);
    vm_write32(LEN, 16);
    assert(C3("getsockname", out, ADDR_B, LEN) == 0 && vm_read16(ADDR_B + 2) == 36659);
    vm_write16(ADDR_A + 8, 36658);               /* vport = the peer's P2P port */
    assert(C3("connect", out, ADDR_A, 16) == 0);
    vm_write32(LEN, 16);
    int32_t in = C3("accept", l1, ADDR_B, LEN);
    assert(in >= 0 && vm_read16(ADDR_B + 2) == 36659 && vm_read16(ADDR_B + 8) == 36659);
    s_p2p_port = 36658;

    /* A stream a peer opened through psnr's relay arrives as a connected host
     * socket: the P2P listener turns readable, and accept hands it out as the
     * peer connecting from its P2P port. */
    {
        struct sockaddr_in la;
        socklen_t ll = sizeof(la);
        host_socket_t hl = socket(AF_INET, SOCK_STREAM, 0), hc, hs;
        memset(&la, 0, sizeof(la));
        la.sin_family = AF_INET;
        la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(hl, (struct sockaddr*)&la, sizeof(la)) == 0 && listen(hl, 1) == 0);
        getsockname(hl, (struct sockaddr*)&la, &ll);
        hc = socket(AF_INET, SOCK_STREAM, 0);
        assert(connect(hc, (struct sockaddr*)&la, sizeof(la)) == 0);
        hs = accept(hl, NULL, NULL);
        host_closesocket(hl);

        const uint8_t peer[4] = { 203, 0, 113, 7 };
        assert(s_sink);   /* the P2P listener registered it */
        s_sink((int64_t)hs, peer, 40001);
        vm_write32(POLLFD, (uint32_t)l1);
        vm_write16(POLLFD + 4, SYS_NET_POLLIN);
        assert(C3("socketpoll", POLLFD, 1, 0) == 1 && (vm_read16(POLLFD + 6) & SYS_NET_POLLIN));
        vm_write32(LEN, 16);
        int32_t rs = C3("accept", l1, ADDR_B, LEN);
        assert(rs >= 0 && vm_read32(ADDR_B + 4) == 0xCB007107u);
        assert(vm_read16(ADDR_B + 2) == 40001 && vm_read16(ADDR_B + 8) == 40001);
        send(hc, "setup", 5, 0);
        assert(call("recv", rs, BUF, 64, 0, 0, 0) == 5 && !memcmp(vm_base + BUF, "setup", 5));
        assert(C1("socketclose", rs) == 0);
        host_closesocket(hc);

        /* The same while the title's thread is already blocked in accept:
         * it must wake for the relayed stream, not wait on the host
         * listener forever. */
        hl = socket(AF_INET, SOCK_STREAM, 0);
        la.sin_port = 0;
        assert(bind(hl, (struct sockaddr*)&la, sizeof(la)) == 0 && listen(hl, 1) == 0);
        ll = sizeof(la);
        getsockname(hl, (struct sockaddr*)&la, &ll);
        hc = socket(AF_INET, SOCK_STREAM, 0);
        assert(connect(hc, (struct sockaddr*)&la, sizeof(la)) == 0);
        s_late.fd = (int64_t)accept(hl, NULL, NULL);
        host_closesocket(hl);
        memcpy(s_late.ip, peer, 4);
        s_late.port = 40002;
        start_late_sink();
        vm_write32(LEN, 16);
        rs = C3("accept", l1, ADDR_B, LEN);   /* blocks until the sink runs */
        assert(rs >= 0 && vm_read16(ADDR_B + 8) == 40002);
        assert(C1("socketclose", rs) == 0);
        host_closesocket(hc);

        /* On a nonblocking listener, the relayed stream is nonblocking too,
         * as a directly accepted one is: a read with nothing waiting says
         * EWOULDBLOCK instead of hanging. */
        vm_write32(OPT, 1);
        assert(C5("setsockopt", l1, SYS_NET_SOL_SOCKET, SYS_NET_SO_NBIO, OPT, 4) == 0);
        hl = socket(AF_INET, SOCK_STREAM, 0);
        la.sin_port = 0;
        assert(bind(hl, (struct sockaddr*)&la, sizeof(la)) == 0 && listen(hl, 1) == 0);
        ll = sizeof(la);
        getsockname(hl, (struct sockaddr*)&la, &ll);
        hc = socket(AF_INET, SOCK_STREAM, 0);
        assert(connect(hc, (struct sockaddr*)&la, sizeof(la)) == 0);
        hs = accept(hl, NULL, NULL);
        host_closesocket(hl);
        s_sink((int64_t)hs, peer, 40003);
        vm_write32(LEN, 16);
        rs = C3("accept", l1, ADDR_B, LEN);
        assert(rs >= 0);
        assert((uint32_t)call("recv", rs, BUF, 64, 0, 0, 0) == 0x80010223u);   /* EWOULDBLOCK */
        assert(C1("socketclose", rs) == 0);
        host_closesocket(hc);
    }
    assert(C1("socketclose", in) == 0 && C1("socketclose", out) == 0);
    assert(C1("socketclose", l2) == 0 && C1("socketclose", l1) == 0);

    /* A bad fd is EBADF (9), and closing works. */
    assert((uint32_t)C1("socketclose", 99) == 0x80010209u && errno_cell() == SYS_NET_EBADF);
    assert(C1("socketclose", a) == 0 && C1("socketclose", b) == 0);
    assert(C1("socketclose", c) == 0 && C1("socketclose", srv) == 0 && C1("socketclose", l) == 0);

    printf("test_sys_net: all passed\n");
    return 0;
}

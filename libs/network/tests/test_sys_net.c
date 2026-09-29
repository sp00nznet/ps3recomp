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

static struct { uint32_t nid; void (*fn)(ppu_context*); } s_reg[64];
static int s_nreg;
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    (void)name;
    s_reg[s_nreg].nid = nid;
    s_reg[s_nreg].fn = fn;
    s_nreg++;
}

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
    assert(call("recvfrom", a, BUF, 64, SYS_NET_MSG_DONTWAIT, 0, 0) == -1);
    assert(errno_cell() == SYS_NET_EWOULDBLOCK);
    vm_write32(OPT, 1);
    assert(C5("setsockopt", a, SYS_NET_SOL_SOCKET, SYS_NET_SO_NBIO, OPT, 4) == 0);
    assert(call("recvfrom", a, BUF, 64, 0, 0, 0) == -1);
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

    /* A bad fd is EBADF (9), and closing works. */
    assert(C1("socketclose", 99) == -1 && errno_cell() == SYS_NET_EBADF);
    assert(C1("socketclose", a) == 0 && C1("socketclose", b) == 0);
    assert(C1("socketclose", c) == 0 && C1("socketclose", srv) == 0 && C1("socketclose", l) == 0);

    printf("test_sys_net: all passed\n");
    return 0;
}

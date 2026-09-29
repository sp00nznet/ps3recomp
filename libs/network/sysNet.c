/*
 * ps3recomp - sys_net module implementation
 *
 * The PS3 BSD socket API (libnet) passed through to host sockets: Winsock2 on
 * Windows, POSIX sockets elsewhere. It is OFF by default. ppu_sysprx.cpp's
 * offline model answers every libnet import unless PS3_NET_ONLINE is set, in
 * which case ps3_net_host_register() puts these handlers in front of it.
 *
 * Every pointer argument is a GUEST address, and everything the guest reads
 * back is big-endian: pollfd events, fd_set words, option values, socklen_t.
 * sockaddr_in needs no swap -- its port and address are already in network
 * order in guest memory, which is exactly what the host struct wants.
 *
 * errno is plain BSD (35 = EWOULDBLOCK). Winsock's WSAE* codes are BSD + 10000,
 * so Windows translates by subtraction; Linux numbers differ and get a table.
 */

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET host_socket_t;
    typedef WSAPOLLFD host_pollfd;
    #define HOST_INVALID_SOCKET INVALID_SOCKET
    #define HOST_SOCKET_ERROR   SOCKET_ERROR
    #define host_closesocket    closesocket
    #define host_poll           WSAPoll
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <netdb.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <poll.h>
    #include <time.h>
    typedef int host_socket_t;
    typedef struct pollfd host_pollfd;
    #define HOST_INVALID_SOCKET (-1)
    #define HOST_SOCKET_ERROR   (-1)
    #define host_closesocket    close
    #define host_poll           poll
#endif

#include "sysNet.h"
#include "../../runtime/ppu/ppu_context.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../include/ps3emu/nid.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

#define EA(p) ((uint32_t)(uintptr_t)(p))

/* PS3-only socket types and options */
#define SYS_NET_SOCK_DGRAM_P2P   6
#define SYS_NET_SOCK_STREAM_P2P  10
#define SYS_NET_SO_USECRYPTO     0x1101
#define SYS_NET_SO_USESIGNATURE  0x1102
#define SYS_NET_SO_TYPE          0x1008
#define SYS_NET_SO_REUSEPORT     0x0200
#define SYS_NET_TCP_NODELAY      1
#define SYS_NET_HOST_NOT_FOUND   1

/* ---------------------------------------------------------------------------
 * State
 * -----------------------------------------------------------------------*/

typedef struct {
    host_socket_t host_fd;
    int           in_use;
    int           nonblocking;
} net_socket_slot;

static net_socket_slot s_sockets[SYS_NET_MAX_SOCKETS];
static int s_net_initialized = 0;

/* Guest scratch, allocated on first use through the allocator the runtime
 * handed ps3_net_host_register. */
static unsigned int (*s_alloc)(unsigned int, unsigned int) = NULL;
static uint32_t s_errno_ea = 0, s_h_errno_ea = 0, s_hostent_ea = 0, s_ntoa_ea = 0;
static int32_t  s_errno = 0;   /* ponytail: one errno for every thread, per-thread cells if a title races on it */

/* hostent scratch layout: struct at +0, h_aliases[] at +32, h_addr_list[] at
 * +40, the address at +48, the name at +64. */
#define HOSTENT_SCRATCH 320u

static uint32_t scratch(uint32_t* ea, uint32_t size)
{
    if (!*ea && s_alloc) *ea = s_alloc(size, 16);
    return *ea;
}

static int32_t fail(int32_t err)
{
    s_errno = err;
    if (s_errno_ea) vm_write32(s_errno_ea, (uint32_t)err);
    return -1;
}

static int32_t host_fail(void)
{
#ifdef _WIN32
    int e = WSAGetLastError();
    /* WSAE* is the BSD value + 10000; a nonblocking connect says WOULDBLOCK
     * where BSD says INPROGRESS, and the caller fixes that one up. */
    if (e >= 10000 && e < 10100) return fail(e - 10000);
    return fail(SYS_NET_EINVAL);
#else
    switch (errno) {
        case EBADF:         return fail(SYS_NET_EBADF);
        case ENOMEM:        return fail(SYS_NET_ENOMEM);
        case EINVAL:        return fail(SYS_NET_EINVAL);
        case EAGAIN:        return fail(SYS_NET_EWOULDBLOCK);
        case EINPROGRESS:   return fail(SYS_NET_EINPROGRESS);
        case EALREADY:      return fail(SYS_NET_EALREADY);
        case ENOTSOCK:      return fail(SYS_NET_ENOTSOCK);
        case EMSGSIZE:      return fail(SYS_NET_EMSGSIZE);
        case EADDRINUSE:    return fail(SYS_NET_EADDRINUSE);
        case EADDRNOTAVAIL: return fail(SYS_NET_EADDRNOTAVAIL);
        case ENETUNREACH:   return fail(SYS_NET_ENETUNREACH);
        case ECONNABORTED:  return fail(SYS_NET_ECONNABORTED);
        case ECONNRESET:    return fail(SYS_NET_ECONNRESET);
        case EISCONN:       return fail(SYS_NET_EISCONN);
        case ENOTCONN:      return fail(SYS_NET_ENOTCONN);
        case ETIMEDOUT:     return fail(SYS_NET_ETIMEDOUT);
        case ECONNREFUSED:  return fail(SYS_NET_ECONNREFUSED);
        case EHOSTUNREACH:  return fail(SYS_NET_EHOSTUNREACH);
        default:            return fail(SYS_NET_EINVAL);
    }
#endif
}

static void net_startup(void)
{
    if (s_net_initialized) return;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    for (int i = 0; i < SYS_NET_MAX_SOCKETS; i++) {
        s_sockets[i].host_fd = HOST_INVALID_SOCKET;
        s_sockets[i].in_use = 0;
    }
    s_net_initialized = 1;
}

/* fd 0 is never handed out: titles use 0 as "no socket" in their own tables. */
static int alloc_slot(host_socket_t fd)
{
    for (int i = 1; i < SYS_NET_MAX_SOCKETS; i++)
        if (!s_sockets[i].in_use) {
            s_sockets[i].host_fd = fd;
            s_sockets[i].in_use = 1;
            s_sockets[i].nonblocking = 0;
            return i;
        }
    return -1;
}

static int valid_socket(int32_t s)
{
    return s > 0 && s < SYS_NET_MAX_SOCKETS && s_sockets[s].in_use;
}

static int read_sockaddr(uint32_t ea, struct sockaddr_in* out)
{
    const uint8_t* p = GUEST_PTR(ea, const uint8_t*);
    if (!p) return -1;
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    memcpy(&out->sin_port, p + 2, 2);   /* network order in both */
    memcpy(&out->sin_addr, p + 4, 4);
    return 0;
}

static void write_sockaddr(uint32_t ea, uint32_t len_ea, const struct sockaddr_in* in)
{
    if (ea) {
        uint8_t* p = GUEST_PTR(ea, uint8_t*);
        memset(p, 0, 16);
        p[0] = 16;
        p[1] = SYS_NET_AF_INET;
        memcpy(p + 2, &in->sin_port, 2);
        memcpy(p + 4, &in->sin_addr, 4);
    }
    if (len_ea) vm_write32(len_ea, 16);
}

static const char* ip_str(const struct in_addr* a, char* buf)
{
    const uint8_t* b = (const uint8_t*)a;
    snprintf(buf, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

static void sleep_ms(int ms)
{
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* MSG_DONTWAIT on a blocking socket. Winsock has no such flag, so both
 * platforms ask poll whether the call would block instead of passing it on. */
static int would_block(int32_t s, int32_t flags, short ev)
{
    if (!(flags & SYS_NET_MSG_DONTWAIT) || s_sockets[s].nonblocking) return 0;
    host_pollfd p;
    p.fd = s_sockets[s].host_fd;
    p.events = ev;
    p.revents = 0;
    return host_poll(&p, 1, 0) == 0;
}

static int set_nonblocking(host_socket_t fd, int on)
{
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

/* ---------------------------------------------------------------------------
 * Init / shutdown
 * -----------------------------------------------------------------------*/

int32_t sys_net_initialize_network_ex(void* param)
{
    (void)param;
    net_startup();
    return CELL_OK;
}

int32_t sys_net_finalize_network(void)
{
    for (int i = 1; i < SYS_NET_MAX_SOCKETS; i++)
        if (s_sockets[i].in_use) {
            host_closesocket(s_sockets[i].host_fd);
            s_sockets[i].in_use = 0;
        }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Sockets
 * -----------------------------------------------------------------------*/

int32_t sys_net_bnet_socket(int32_t domain, int32_t type, int32_t protocol)
{
    net_startup();
    if (domain != SYS_NET_AF_INET) return fail(SYS_NET_EINVAL);

    int host_type;
    switch (type) {
        case SYS_NET_SOCK_STREAM:
        case SYS_NET_SOCK_STREAM_P2P: host_type = SOCK_STREAM; break;
        case SYS_NET_SOCK_DGRAM:
        /* ponytail: P2P sockets are plain UDP/TCP here -- no vport multiplexing
         * over one port. Enough while both ends are this runtime; the signaling
         * layer is where real vports would go. */
        case SYS_NET_SOCK_DGRAM_P2P:  host_type = SOCK_DGRAM;  break;
        default: return fail(SYS_NET_EINVAL);
    }

    host_socket_t fd = socket(AF_INET, host_type, (type >= SYS_NET_SOCK_DGRAM_P2P) ? 0 : protocol);
    if (fd == HOST_INVALID_SOCKET) return host_fail();

    int slot = alloc_slot(fd);
    if (slot < 0) {
        host_closesocket(fd);
        return fail(SYS_NET_ENOMEM);
    }
    printf("[sys_net] socket(%d, %d, %d) -> %d\n", domain, type, protocol, slot);
    return slot;
}

int32_t sys_net_bnet_close(int32_t s)
{
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    host_closesocket(s_sockets[s].host_fd);
    s_sockets[s].in_use = 0;
    return 0;
}

int32_t sys_net_bnet_bind(int32_t s, const sys_net_sockaddr* addr, uint32_t addrlen)
{
    (void)addrlen;
    struct sockaddr_in a;
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (read_sockaddr(EA(addr), &a)) return fail(SYS_NET_EINVAL);
    if (bind(s_sockets[s].host_fd, (struct sockaddr*)&a, sizeof(a)) == HOST_SOCKET_ERROR)
        return host_fail();
    printf("[sys_net] bind(%d, port %u)\n", s, ntohs(a.sin_port));
    return 0;
}

int32_t sys_net_bnet_listen(int32_t s, int32_t backlog)
{
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (listen(s_sockets[s].host_fd, backlog) == HOST_SOCKET_ERROR) return host_fail();
    return 0;
}

int32_t sys_net_bnet_accept(int32_t s, sys_net_sockaddr* addr, uint32_t* addrlen)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);

    host_socket_t fd = accept(s_sockets[s].host_fd, (struct sockaddr*)&a, &len);
    if (fd == HOST_INVALID_SOCKET) return host_fail();

    int slot = alloc_slot(fd);
    if (slot < 0) {
        host_closesocket(fd);
        return fail(SYS_NET_ENOMEM);
    }
    write_sockaddr(EA(addr), EA(addrlen), &a);
    return slot;
}

int32_t sys_net_bnet_connect(int32_t s, const sys_net_sockaddr* addr, uint32_t addrlen)
{
    (void)addrlen;
    struct sockaddr_in a;
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (read_sockaddr(EA(addr), &a)) return fail(SYS_NET_EINVAL);

    char ip[16];
    printf("[sys_net] connect(%d, %s:%u)\n", s, ip_str(&a.sin_addr, ip), ntohs(a.sin_port));
    if (connect(s_sockets[s].host_fd, (struct sockaddr*)&a, sizeof(a)) == HOST_SOCKET_ERROR) {
#ifdef _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK) return fail(SYS_NET_EINPROGRESS);
#endif
        return host_fail();
    }
    return 0;
}

int32_t sys_net_bnet_shutdown(int32_t s, int32_t how)
{
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (shutdown(s_sockets[s].host_fd, how) == HOST_SOCKET_ERROR) return host_fail();
    return 0;
}

/* ---------------------------------------------------------------------------
 * Data transfer
 * -----------------------------------------------------------------------*/

static int host_recv_flags(int32_t flags)
{
    int f = 0;
    if (flags & SYS_NET_MSG_PEEK)    f |= MSG_PEEK;
    if (flags & SYS_NET_MSG_WAITALL) f |= MSG_WAITALL;
    return f;
}

int32_t sys_net_bnet_send(int32_t s, const void* buf, uint32_t len, int32_t flags)
{
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (would_block(s, flags, POLLOUT)) return fail(SYS_NET_EWOULDBLOCK);
    int n = send(s_sockets[s].host_fd, GUEST_PTR(EA(buf), const char*), (int)len, 0);
    return n == HOST_SOCKET_ERROR ? host_fail() : n;
}

int32_t sys_net_bnet_sendto(int32_t s, const void* buf, uint32_t len, int32_t flags,
                            const sys_net_sockaddr* to, uint32_t tolen)
{
    (void)tolen;
    struct sockaddr_in a;
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (!to) return sys_net_bnet_send(s, buf, len, flags);   /* connected socket */
    if (read_sockaddr(EA(to), &a)) return fail(SYS_NET_EINVAL);
    if (would_block(s, flags, POLLOUT)) return fail(SYS_NET_EWOULDBLOCK);
    int n = sendto(s_sockets[s].host_fd, GUEST_PTR(EA(buf), const char*), (int)len, 0,
                   (struct sockaddr*)&a, sizeof(a));
    return n == HOST_SOCKET_ERROR ? host_fail() : n;
}

int32_t sys_net_bnet_recv(int32_t s, void* buf, uint32_t len, int32_t flags)
{
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (would_block(s, flags, POLLIN)) return fail(SYS_NET_EWOULDBLOCK);
    int n = recv(s_sockets[s].host_fd, GUEST_PTR(EA(buf), char*), (int)len, host_recv_flags(flags));
    return n == HOST_SOCKET_ERROR ? host_fail() : n;
}

int32_t sys_net_bnet_recvfrom(int32_t s, void* buf, uint32_t len, int32_t flags,
                              sys_net_sockaddr* from, uint32_t* fromlen)
{
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (would_block(s, flags, POLLIN)) return fail(SYS_NET_EWOULDBLOCK);

    memset(&a, 0, sizeof(a));
    int n = recvfrom(s_sockets[s].host_fd, GUEST_PTR(EA(buf), char*), (int)len,
                     host_recv_flags(flags), (struct sockaddr*)&a, &alen);
    if (n == HOST_SOCKET_ERROR) return host_fail();
    write_sockaddr(EA(from), EA(fromlen), &a);
    return n;
}

/* ---------------------------------------------------------------------------
 * Socket options
 * -----------------------------------------------------------------------*/

/* Timeouts arrive as a PS3 timeval {s64 sec, s64 usec}. */
static int set_timeout(host_socket_t fd, int name, uint32_t val_ea, uint32_t len)
{
    uint64_t us = (len >= 16) ? vm_read64(val_ea) * 1000000ull + vm_read64(val_ea + 8)
                              : (uint64_t)vm_read32(val_ea) * 1000ull;
#ifdef _WIN32
    DWORD ms = (DWORD)(us / 1000);
    return setsockopt(fd, SOL_SOCKET, name, (const char*)&ms, sizeof(ms));
#else
    struct timeval tv = { (time_t)(us / 1000000), (suseconds_t)(us % 1000000) };
    return setsockopt(fd, SOL_SOCKET, name, &tv, sizeof(tv));
#endif
}

int32_t sys_net_bnet_setsockopt(int32_t s, int32_t level, int32_t optname,
                                const void* optval, uint32_t optlen)
{
    uint32_t v_ea = EA(optval);
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    host_socket_t fd = s_sockets[s].host_fd;
    int val = (v_ea && optlen >= 4) ? (int)vm_read32(v_ea) : 0;
    int ret = 0;

    if (level == SYS_NET_SOL_SOCKET) {
        switch (optname) {
            case SYS_NET_SO_NBIO:
                s_sockets[s].nonblocking = val != 0;
                ret = set_nonblocking(fd, val);
                break;
            case SYS_NET_SO_REUSEADDR:
            case SYS_NET_SO_REUSEPORT:
                ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&val, sizeof(val)); break;
            case SYS_NET_SO_KEEPALIVE:
                ret = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&val, sizeof(val)); break;
            case SYS_NET_SO_BROADCAST:
                ret = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&val, sizeof(val)); break;
            case SYS_NET_SO_SNDBUF:
                ret = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&val, sizeof(val)); break;
            case SYS_NET_SO_RCVBUF:
                ret = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&val, sizeof(val)); break;
            case SYS_NET_SO_SNDTIMEO: ret = set_timeout(fd, SO_SNDTIMEO, v_ea, optlen); break;
            case SYS_NET_SO_RCVTIMEO: ret = set_timeout(fd, SO_RCVTIMEO, v_ea, optlen); break;
            case SYS_NET_SO_LINGER: {
                struct linger l;
                l.l_onoff  = (unsigned short)vm_read32(v_ea);
                l.l_linger = (unsigned short)vm_read32(v_ea + 4);
                ret = setsockopt(fd, SOL_SOCKET, SO_LINGER, (const char*)&l, sizeof(l));
                break;
            }
            /* NP signaling's packet crypto/signature: meaningless off-console. */
            case SYS_NET_SO_USECRYPTO:
            case SYS_NET_SO_USESIGNATURE:
                break;
            default:
                printf("[sys_net] setsockopt(%d, SOL_SOCKET, 0x%X) ignored\n", s, (unsigned)optname);
                break;
        }
    } else if (level == SYS_NET_IPPROTO_TCP && optname == SYS_NET_TCP_NODELAY) {
        ret = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&val, sizeof(val));
    } else {
        /* ponytail: IP-level options (TTL, TOS, multicast) are accepted and
         * dropped; map them when a title's traffic depends on one. */
        printf("[sys_net] setsockopt(%d, level %d, 0x%X) ignored\n", s, level, (unsigned)optname);
    }
    return ret == HOST_SOCKET_ERROR ? host_fail() : 0;
}

int32_t sys_net_bnet_getsockopt(int32_t s, int32_t level, int32_t optname,
                                void* optval, uint32_t* optlen)
{
    uint32_t v_ea = EA(optval), len_ea = EA(optlen);
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    host_socket_t fd = s_sockets[s].host_fd;
    int val = 0;
    socklen_t hl = sizeof(val);

    if (level == SYS_NET_SOL_SOCKET) {
        switch (optname) {
            case SYS_NET_SO_NBIO: val = s_sockets[s].nonblocking; break;
            case SYS_NET_SO_ERROR:
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&val, &hl) == HOST_SOCKET_ERROR)
                    return host_fail();
#ifdef _WIN32
                if (val >= 10000 && val < 10100) val -= 10000;
#else
                if (val == ECONNREFUSED) val = SYS_NET_ECONNREFUSED;
                else if (val == ETIMEDOUT) val = SYS_NET_ETIMEDOUT;
                else if (val == ECONNRESET) val = SYS_NET_ECONNRESET;
                else if (val == EHOSTUNREACH) val = SYS_NET_EHOSTUNREACH;
#endif
                break;
            case SYS_NET_SO_TYPE:
                if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (char*)&val, &hl) == HOST_SOCKET_ERROR)
                    return host_fail();
                val = (val == SOCK_STREAM) ? SYS_NET_SOCK_STREAM : SYS_NET_SOCK_DGRAM;
                break;
            case SYS_NET_SO_SNDBUF:
            case SYS_NET_SO_RCVBUF:
                if (getsockopt(fd, SOL_SOCKET, optname == SYS_NET_SO_SNDBUF ? SO_SNDBUF : SO_RCVBUF,
                               (char*)&val, &hl) == HOST_SOCKET_ERROR)
                    return host_fail();
                break;
            default: break;   /* everything else reads as 0 */
        }
    }
    if (v_ea) vm_write32(v_ea, (uint32_t)val);
    if (len_ea) vm_write32(len_ea, 4);
    return 0;
}

int32_t sys_net_bnet_getsockname(int32_t s, sys_net_sockaddr* addr, uint32_t* addrlen)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (getsockname(s_sockets[s].host_fd, (struct sockaddr*)&a, &len) == HOST_SOCKET_ERROR)
        return host_fail();
    write_sockaddr(EA(addr), EA(addrlen), &a);
    return 0;
}

int32_t sys_net_bnet_getpeername(int32_t s, sys_net_sockaddr* addr, uint32_t* addrlen)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (getpeername(s_sockets[s].host_fd, (struct sockaddr*)&a, &len) == HOST_SOCKET_ERROR)
        return host_fail();
    write_sockaddr(EA(addr), EA(addrlen), &a);
    return 0;
}

/* ---------------------------------------------------------------------------
 * poll / select
 *
 * Both go through one host poll. Guest pollfd is {s32 fd, s16 events,
 * s16 revents}; guest fd_set is 1024 bits in big-endian u32 words, fd n at
 * bit (n % 32) of word (n / 32) -- the BSD fd_mask layout.
 * -----------------------------------------------------------------------*/

#define NET_POLL_MAX 64

/* Run host poll over n guest fds. Invalid fds come back POLLNVAL without
 * reaching the host (WSAPoll rejects the whole call over one bad handle). */
static int run_poll(const int32_t* gfd, const short* gev, short* grev, int n, int timeout_ms)
{
    host_pollfd hp[NET_POLL_MAX];
    int map[NET_POLL_MAX], nh = 0, nval = 0;

    for (int i = 0; i < n; i++) {
        grev[i] = 0;
        if (!valid_socket(gfd[i])) { grev[i] = SYS_NET_POLLNVAL; nval++; continue; }
        hp[nh].fd = s_sockets[gfd[i]].host_fd;
        hp[nh].events = 0;
        if (gev[i] & SYS_NET_POLLIN)  hp[nh].events |= POLLIN;
        if (gev[i] & SYS_NET_POLLOUT) hp[nh].events |= POLLOUT;
        hp[nh].revents = 0;
        map[nh++] = i;
    }
    if (nval) timeout_ms = 0;
    if (nh == 0) { sleep_ms(timeout_ms); return nval; }

    int r = host_poll(hp, (unsigned)nh, timeout_ms);
    if (r == HOST_SOCKET_ERROR) return host_fail();

    int ready = nval;
    for (int k = 0; k < nh; k++) {
        short rv = 0, h = hp[k].revents;
        if (h & POLLIN)   rv |= SYS_NET_POLLIN;
        if (h & POLLOUT)  rv |= SYS_NET_POLLOUT;
        if (h & POLLERR)  rv |= SYS_NET_POLLERR;
        if (h & POLLHUP)  rv |= SYS_NET_POLLHUP;
        if (h & POLLNVAL) rv |= SYS_NET_POLLNVAL;
        grev[map[k]] = rv;
        if (rv) ready++;
    }
    return ready;
}

int32_t sys_net_bnet_poll(sys_net_pollfd* fds, uint32_t nfds, int32_t timeout_ms)
{
    int32_t gfd[NET_POLL_MAX];
    short gev[NET_POLL_MAX], grev[NET_POLL_MAX];
    uint32_t base = EA(fds);
    if (nfds > NET_POLL_MAX) return fail(SYS_NET_EINVAL);
    if (!base && nfds) return fail(SYS_NET_EINVAL);

    for (uint32_t i = 0; i < nfds; i++) {
        gfd[i] = (int32_t)vm_read32(base + i * 8);
        gev[i] = (short)vm_read16(base + i * 8 + 4);
    }
    int r = run_poll(gfd, gev, grev, (int)nfds, timeout_ms);
    if (r < 0) return r;
    for (uint32_t i = 0; i < nfds; i++) vm_write16(base + i * 8 + 6, (uint16_t)grev[i]);
    return r;
}

int32_t sys_net_bnet_select(int32_t nfds, void* readfds, void* writefds,
                            void* exceptfds, void* timeout)
{
    uint32_t rd = EA(readfds), wr = EA(writefds), ex = EA(exceptfds), tv = EA(timeout);
    int32_t gfd[NET_POLL_MAX];
    short gev[NET_POLL_MAX], grev[NET_POLL_MAX];
    int n = 0;
    if (nfds > 1024) nfds = 1024;

    for (int32_t fd = 0; fd < nfds; fd++) {
        uint32_t word = (uint32_t)fd / 32 * 4, bit = 1u << (fd % 32);
        short ev = 0;
        if (rd && (vm_read32(rd + word) & bit)) ev |= SYS_NET_POLLIN;
        if (wr && (vm_read32(wr + word) & bit)) ev |= SYS_NET_POLLOUT;
        if (!ev) continue;
        if (n == NET_POLL_MAX) return fail(SYS_NET_EINVAL);
        gfd[n] = fd; gev[n] = ev; n++;
    }

    int timeout_ms = -1;   /* NULL timeval = wait forever */
    if (tv) {
        uint64_t us = vm_read64(tv) * 1000000ull + vm_read64(tv + 8);
        timeout_ms = (int)((us + 999) / 1000);
    }
    int r = run_poll(gfd, gev, grev, n, timeout_ms);
    if (r < 0) return r;

    /* Rewrite the sets to hold only what is ready; the except set never is. */
    for (uint32_t i = 0; i < 128; i += 4) {
        if (rd) vm_write32(rd + i, 0);
        if (wr) vm_write32(wr + i, 0);
        if (ex) vm_write32(ex + i, 0);
    }
    int count = 0;
    for (int i = 0; i < n; i++) {
        uint32_t word = (uint32_t)gfd[i] / 32 * 4, bit = 1u << (gfd[i] % 32);
        short in_ready = SYS_NET_POLLIN | SYS_NET_POLLHUP | SYS_NET_POLLERR;
        if ((gev[i] & SYS_NET_POLLIN) && (grev[i] & in_ready)) {
            vm_write32(rd + word, vm_read32(rd + word) | bit); count++;
        }
        if ((gev[i] & SYS_NET_POLLOUT) && (grev[i] & (SYS_NET_POLLOUT | SYS_NET_POLLERR))) {
            vm_write32(wr + word, vm_read32(wr + word) | bit); count++;
        }
    }
    return count;
}

/* ---------------------------------------------------------------------------
 * Addresses and DNS
 * -----------------------------------------------------------------------*/

int32_t sys_net_bnet_inet_aton(const char* cp, uint32_t* inp)
{
    struct in_addr a;
    const char* s = GUEST_PTR(EA(cp), const char*);
    if (!s || inet_pton(AF_INET, s, &a) != 1) return 0;
    if (inp) memcpy(vm_ptr8(EA(inp)), &a, 4);
    return 1;
}

uint32_t sys_net_bnet_inet_addr(const char* cp)
{
    struct in_addr a;
    const char* s = GUEST_PTR(EA(cp), const char*);
    if (!s || inet_pton(AF_INET, s, &a) != 1) return 0xFFFFFFFFu;   /* INADDR_NONE */
    return ntohl(a.s_addr);
}

uint32_t sys_net_bnet_inet_ntoa(uint32_t addr)
{
    uint32_t ea = scratch(&s_ntoa_ea, 16);
    if (!ea) return 0;
    snprintf((char*)vm_ptr8(ea), 16, "%u.%u.%u.%u",
             addr >> 24, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF);
    return ea;
}

uint32_t sys_net_bnet_gethostbyname(const char* name)
{
    const char* host = GUEST_PTR(EA(name), const char*);
    uint32_t h = scratch(&s_hostent_ea, HOSTENT_SCRATCH);
    struct addrinfo hints, *res = NULL;
    if (!host || !h) return 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        printf("[sys_net] gethostbyname('%s') failed\n", host);
        if (scratch(&s_h_errno_ea, 4)) vm_write32(s_h_errno_ea, SYS_NET_HOST_NOT_FOUND);
        return 0;
    }
    const struct sockaddr_in* sa = (const struct sockaddr_in*)res->ai_addr;

    memset(vm_ptr8(h), 0, HOSTENT_SCRATCH);
    vm_write32(h + 0,  h + 64);           /* h_name */
    vm_write32(h + 4,  h + 32);           /* h_aliases -> { NULL } */
    vm_write32(h + 8,  SYS_NET_AF_INET);  /* h_addrtype */
    vm_write32(h + 12, 4);                /* h_length */
    vm_write32(h + 16, h + 40);           /* h_addr_list -> { &addr, NULL } */
    vm_write32(h + 40, h + 48);
    memcpy(vm_ptr8(h + 48), &sa->sin_addr, 4);
    strncpy((char*)vm_ptr8(h + 64), host, HOSTENT_SCRATCH - 65);

    char ip[16];
    printf("[sys_net] gethostbyname('%s') -> %s\n", host, ip_str(&sa->sin_addr, ip));
    freeaddrinfo(res);
    return h;
}

uint32_t sys_net_errno_loc(void)
{
    if (scratch(&s_errno_ea, 4)) vm_write32(s_errno_ea, (uint32_t)s_errno);
    return s_errno_ea;
}

/* ---------------------------------------------------------------------------
 * Registration: libnet export names -> ctx handlers
 * -----------------------------------------------------------------------*/

#define R32(n)  ((uint32_t)ctx->gpr[n])
#define S32(n)  ((int32_t)(uint32_t)ctx->gpr[n])
#define PTR(n)  ((void*)(uintptr_t)(uint32_t)ctx->gpr[n])
#define RET_S(v) (ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)(v))
#define RET_U(v) (ctx->gpr[3] = (uint64_t)(uint32_t)(v))

static void h_socket(ppu_context* ctx)      { RET_S(sys_net_bnet_socket(S32(3), S32(4), S32(5))); }
static void h_close(ppu_context* ctx)       { RET_S(sys_net_bnet_close(S32(3))); }
static void h_bind(ppu_context* ctx)        { RET_S(sys_net_bnet_bind(S32(3), PTR(4), R32(5))); }
static void h_listen(ppu_context* ctx)      { RET_S(sys_net_bnet_listen(S32(3), S32(4))); }
static void h_accept(ppu_context* ctx)      { RET_S(sys_net_bnet_accept(S32(3), PTR(4), PTR(5))); }
static void h_connect(ppu_context* ctx)     { RET_S(sys_net_bnet_connect(S32(3), PTR(4), R32(5))); }
static void h_shutdown(ppu_context* ctx)    { RET_S(sys_net_bnet_shutdown(S32(3), S32(4))); }
static void h_send(ppu_context* ctx)        { RET_S(sys_net_bnet_send(S32(3), PTR(4), R32(5), S32(6))); }
static void h_sendto(ppu_context* ctx)      { RET_S(sys_net_bnet_sendto(S32(3), PTR(4), R32(5), S32(6), PTR(7), R32(8))); }
static void h_recv(ppu_context* ctx)        { RET_S(sys_net_bnet_recv(S32(3), PTR(4), R32(5), S32(6))); }
static void h_recvfrom(ppu_context* ctx)    { RET_S(sys_net_bnet_recvfrom(S32(3), PTR(4), R32(5), S32(6), PTR(7), PTR(8))); }
static void h_setsockopt(ppu_context* ctx)  { RET_S(sys_net_bnet_setsockopt(S32(3), S32(4), S32(5), PTR(6), R32(7))); }
static void h_getsockopt(ppu_context* ctx)  { RET_S(sys_net_bnet_getsockopt(S32(3), S32(4), S32(5), PTR(6), PTR(7))); }
static void h_getsockname(ppu_context* ctx) { RET_S(sys_net_bnet_getsockname(S32(3), PTR(4), PTR(5))); }
static void h_getpeername(ppu_context* ctx) { RET_S(sys_net_bnet_getpeername(S32(3), PTR(4), PTR(5))); }
static void h_poll(ppu_context* ctx)        { RET_S(sys_net_bnet_poll(PTR(3), R32(4), S32(5))); }
static void h_select(ppu_context* ctx)      { RET_S(sys_net_bnet_select(S32(3), PTR(4), PTR(5), PTR(6), PTR(7))); }
static void h_inet_aton(ppu_context* ctx)   { RET_S(sys_net_bnet_inet_aton(PTR(3), PTR(4))); }
static void h_inet_addr(ppu_context* ctx)   { RET_U(sys_net_bnet_inet_addr(PTR(3))); }
static void h_inet_ntoa(ppu_context* ctx)   { RET_U(sys_net_bnet_inet_ntoa(R32(3))); }
static void h_gethostbyname(ppu_context* ctx) { RET_U(sys_net_bnet_gethostbyname(PTR(3))); }
static void h_errno_loc(ppu_context* ctx)   { RET_U(sys_net_errno_loc()); }
static void h_h_errno_loc(ppu_context* ctx) { RET_U(scratch(&s_h_errno_ea, 4)); }
static void h_init(ppu_context* ctx)        { RET_S(sys_net_initialize_network_ex(PTR(3))); }
static void h_finalize(ppu_context* ctx)    { RET_S(sys_net_finalize_network()); }
static void h_ok(ppu_context* ctx)          { RET_S(0); }

void ps3_net_host_register(unsigned int (*guest_alloc)(unsigned int size, unsigned int align))
{
    static const struct { const char* name; void (*fn)(ppu_context*); } tab[] = {
        { "socket", h_socket },           { "socketclose", h_close },
        { "bind", h_bind },               { "listen", h_listen },
        { "accept", h_accept },           { "connect", h_connect },
        { "shutdown", h_shutdown },       { "send", h_send },
        { "sendto", h_sendto },           { "recv", h_recv },
        { "recvfrom", h_recvfrom },       { "setsockopt", h_setsockopt },
        { "getsockopt", h_getsockopt },   { "getsockname", h_getsockname },
        { "getpeername", h_getpeername }, { "socketpoll", h_poll },
        { "socketselect", h_select },     { "inet_aton", h_inet_aton },
        { "inet_addr", h_inet_addr },     { "inet_ntoa", h_inet_ntoa },
        { "gethostbyname", h_gethostbyname },
        { "_sys_net_errno_loc", h_errno_loc },
        { "_sys_net_h_errno_loc", h_h_errno_loc },
        { "sys_net_initialize_network_ex", h_init },
        { "sys_net_finalize_network", h_finalize },
        { "sys_net_free_thread_context", h_ok },
        { "sys_net_abort_resolver", h_ok },
    };
    s_alloc = guest_alloc;
    net_startup();
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        ps3_hle_register_ctx(ps3_compute_nid(tab[i].name), tab[i].name, tab[i].fn);
    printf("[sys_net] PS3_NET_ONLINE: host sockets registered (%u exports)\n",
           (unsigned)(sizeof(tab) / sizeof(tab[0])));
}

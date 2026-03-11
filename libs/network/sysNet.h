/*
 * ps3recomp - sys_net module
 *
 * BSD socket API for PS3 games. Wraps the host OS socket layer
 * (Winsock on Windows, POSIX sockets on Linux/macOS) to provide
 * the PS3 sys_net interface.
 *
 * PS3 uses its own socket descriptor space and error codes.
 * This module translates between PS3 and host conventions.
 */

#ifndef SYS_NET_H
#define SYS_NET_H

#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * PS3 network error codes
 * -----------------------------------------------------------------------*/
#define SYS_NET_ERROR_BASE          0x80410100
#define SYS_NET_EWOULDBLOCK         (SYS_NET_ERROR_BASE | 0x03)
#define SYS_NET_EINPROGRESS         (SYS_NET_ERROR_BASE | 0x06)
#define SYS_NET_EALREADY            (SYS_NET_ERROR_BASE | 0x07)
#define SYS_NET_ENOTCONN            (SYS_NET_ERROR_BASE | 0x0B)
#define SYS_NET_ECONNREFUSED        (SYS_NET_ERROR_BASE | 0x13)
#define SYS_NET_ETIMEDOUT           (SYS_NET_ERROR_BASE | 0x16)
#define SYS_NET_ECONNRESET          (SYS_NET_ERROR_BASE | 0x15)
#define SYS_NET_ECONNABORTED        (SYS_NET_ERROR_BASE | 0x14)
#define SYS_NET_ENOMEM              (SYS_NET_ERROR_BASE | 0x23)
#define SYS_NET_EBADF               (SYS_NET_ERROR_BASE | 0x27)
#define SYS_NET_EINVAL              (SYS_NET_ERROR_BASE | 0x28)

/* PS3 address families */
#define SYS_NET_AF_INET             2

/* PS3 socket types */
#define SYS_NET_SOCK_STREAM         1
#define SYS_NET_SOCK_DGRAM          2

/* PS3 IPPROTO */
#define SYS_NET_IPPROTO_IP          0
#define SYS_NET_IPPROTO_TCP         6
#define SYS_NET_IPPROTO_UDP         17

/* PS3 socket options */
#define SYS_NET_SOL_SOCKET          0xFFFF
#define SYS_NET_SO_REUSEADDR        0x0004
#define SYS_NET_SO_KEEPALIVE        0x0008
#define SYS_NET_SO_BROADCAST        0x0020
#define SYS_NET_SO_LINGER           0x0080
#define SYS_NET_SO_SNDBUF           0x1001
#define SYS_NET_SO_RCVBUF           0x1002
#define SYS_NET_SO_SNDTIMEO         0x1005
#define SYS_NET_SO_RCVTIMEO         0x1006
#define SYS_NET_SO_ERROR            0x1007
#define SYS_NET_SO_NBIO             0x1100  /* PS3-specific non-blocking */

/* PS3 MSG flags */
#define SYS_NET_MSG_PEEK            0x02
#define SYS_NET_MSG_DONTWAIT        0x80
#define SYS_NET_MSG_WAITALL         0x40

/* PS3 poll events */
#define SYS_NET_POLLIN              0x0001
#define SYS_NET_POLLOUT             0x0004
#define SYS_NET_POLLERR             0x0008
#define SYS_NET_POLLHUP             0x0010
#define SYS_NET_POLLNVAL            0x0020

/* Max sockets */
#define SYS_NET_MAX_SOCKETS         128

/* ---------------------------------------------------------------------------
 * PS3 network structures (big-endian on PS3, host-endian in our impl)
 * -----------------------------------------------------------------------*/

/* PS3 sockaddr_in — identical layout to BSD but fields are big-endian */
typedef struct sys_net_sockaddr_in {
    uint8_t  sin_len;       /* length */
    uint8_t  sin_family;    /* AF_INET */
    uint16_t sin_port;      /* port (network byte order) */
    uint32_t sin_addr;      /* IPv4 address (network byte order) */
    uint8_t  sin_zero[8];
} sys_net_sockaddr_in;

typedef struct sys_net_sockaddr {
    uint8_t  sa_len;
    uint8_t  sa_family;
    uint8_t  sa_data[14];
} sys_net_sockaddr;

typedef struct sys_net_linger {
    int32_t l_onoff;
    int32_t l_linger;
} sys_net_linger;

typedef struct sys_net_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
} sys_net_pollfd;

/* PS3 hostent */
typedef struct sys_net_hostent {
    uint32_t h_name;        /* guest pointer to name string */
    uint32_t h_aliases;     /* guest pointer to aliases array */
    int32_t  h_addrtype;
    int32_t  h_length;
    uint32_t h_addr_list;   /* guest pointer to address list */
} sys_net_hostent;

/* ---------------------------------------------------------------------------
 * API functions (matching PS3 NID exports from sys_net)
 * -----------------------------------------------------------------------*/

/* Network init/shutdown */
int32_t sys_net_initialize_network_ex(uint32_t mem_container, uint32_t flags);
int32_t sys_net_finalize_network(void);

/* BSD socket API */
int32_t sys_net_bnet_socket(int32_t domain, int32_t type, int32_t protocol);
int32_t sys_net_bnet_bind(int32_t s, const sys_net_sockaddr* addr, uint32_t addrlen);
int32_t sys_net_bnet_listen(int32_t s, int32_t backlog);
int32_t sys_net_bnet_accept(int32_t s, sys_net_sockaddr* addr, uint32_t* addrlen);
int32_t sys_net_bnet_connect(int32_t s, const sys_net_sockaddr* addr, uint32_t addrlen);
int32_t sys_net_bnet_send(int32_t s, const void* buf, uint32_t len, int32_t flags);
int32_t sys_net_bnet_sendto(int32_t s, const void* buf, uint32_t len, int32_t flags,
                            const sys_net_sockaddr* to, uint32_t tolen);
int32_t sys_net_bnet_recv(int32_t s, void* buf, uint32_t len, int32_t flags);
int32_t sys_net_bnet_recvfrom(int32_t s, void* buf, uint32_t len, int32_t flags,
                              sys_net_sockaddr* from, uint32_t* fromlen);
int32_t sys_net_bnet_shutdown(int32_t s, int32_t how);
int32_t sys_net_bnet_close(int32_t s);

/* Socket options */
int32_t sys_net_bnet_setsockopt(int32_t s, int32_t level, int32_t optname,
                                const void* optval, uint32_t optlen);
int32_t sys_net_bnet_getsockopt(int32_t s, int32_t level, int32_t optname,
                                void* optval, uint32_t* optlen);
int32_t sys_net_bnet_getsockname(int32_t s, sys_net_sockaddr* addr, uint32_t* addrlen);

/* I/O multiplexing */
int32_t sys_net_bnet_poll(sys_net_pollfd* fds, uint32_t nfds, int32_t timeout_ms);
int32_t sys_net_bnet_select(int32_t nfds, void* readfds, void* writefds,
                            void* exceptfds, void* timeout);

/* DNS */
int32_t sys_net_bnet_inet_aton(const char* cp, uint32_t* inp);
/* gethostbyname returns a guest pointer to a static hostent */
uint32_t sys_net_bnet_gethostbyname(const char* name);

/* Thread-local errno */
int32_t* sys_net_errno_loc(void);

#ifdef __cplusplus
}
#endif

#endif /* SYS_NET_H */

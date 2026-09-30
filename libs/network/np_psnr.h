/*
 * ps3recomp - the connection to a psnr server (libs/network/psnr/)
 *
 * Online NP needs three things set:
 *   PS3_NET_ONLINE=1         real sockets and a connected cellNetCtl
 *   PSNR_SERVER=host[:port]  the psnr server (port 36100 by default)
 *   PS3_NP_ONLINE_ID=name    optional; see np_psnr_setup for the default
 * and optionally PS3_NET_P2P_PORT (default 3658), the host port this
 * instance's P2P sockets bind. That port goes to peers through psnr; two
 * instances on one machine need different values.
 *
 * One connection per process, made when the first NP context starts.
 * Requests take a reply handler. np_psnr_pump(), called from
 * cellSysutilCheckCallback, runs reply and push handlers on the title's own
 * polling thread. That is where they queue guest callbacks, and the same
 * CheckCallback call delivers them.
 */
#ifndef PS3RECOMP_NP_PSNR_H
#define PS3RECOMP_NP_PSNR_H

#include "psnr/psnr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PS3_NET_ONLINE and PSNR_SERVER are both set. */
int np_psnr_enabled(void);

/* Connect (once) as this title. 0 if connected, -1 if not. */
int np_psnr_connect(const char* comm_id);
int np_psnr_connected(void);

/* Settle the player's name and server at startup, before the title runs; the
 * runtime's main passes --username and --psnr (either may be NULL).
 *   --psnr host[:port]  sets PS3_NET_ONLINE=1 and PSNR_SERVER.
 *   The name is --username, else PS3_NP_ONLINE_ID, else -- online only -- the
 *   OS login name. Offline with neither, nothing changes ("PS3Player").
 * Names follow PSN's online ID rules: 3-16 letters, digits, '-' or '_',
 * starting with a letter. */
void np_psnr_setup(const char* username, const char* server);

/* The name the NP ID, psnr and the console identity (OpenPSID, MAC) derive
 * from, or NULL offline with no name set -- then the old fixed values stand,
 * so offline saves are untouched. Resolves from the environment if main never
 * called np_psnr_setup. */
const char* np_psnr_identity(void);

/* The player's online ID, and the P2P port this instance binds and advertises. */
const char* np_psnr_online_id(void);
uint16_t    np_psnr_p2p_port(void);

/* Send a request; fn(user, reply) runs from np_psnr_pump(). A reply of
 * type 0 means the connection dropped before it came. 0 if not sent. */
typedef void (*np_psnr_reply_fn)(void* user, const psnr_msg* reply);
uint32_t np_psnr_request(uint8_t type, const void* body, uint32_t len,
                         np_psnr_reply_fn fn, void* user);

/* Where server pushes go (room events). One handler: Matching2. */
void np_psnr_on_push(void (*fn)(const psnr_msg* push));

/* Called at the start of every np_psnr_pump(), for work that falls due over
 * time rather than on a message (Matching2's deferred signaling). */
void np_psnr_on_tick(void (*fn)(void));

void np_psnr_pump(void);

/* NAT traversal (psnr docs/api.md, "UDP"). A player behind a router has a
 * public P2P port the server can't see over TCP, so np_psnr probes psnr from
 * the title's own P2P datagram socket -- psnr then hands peers on other
 * networks that socket's public endpoint -- and punches toward each peer so
 * this side's router lets the peer's traffic in. sysNet owns the socket:
 *   - it registers how to send from it once the title binds it (NULL when it
 *     closes); probes and punches go out from np_psnr_pump;
 *   - it passes every datagram that arrives on it to np_psnr_p2p_filter,
 *     which consumes psnr's own packets: the title never sees them. */
typedef int (*np_psnr_p2p_send_fn)(const void* buf, uint32_t len,
                                   const uint8_t ip[4], uint16_t port);
void np_psnr_set_p2p_sender(np_psnr_p2p_send_fn fn);

/* A datagram that arrived on the P2P socket from from_ip:from_port. 1: it was
 * psnr's own (a probe reply, a peer's punch) -- drop it. 0: it's the title's.
 * A datagram a peer sent through the relay is unwrapped in place: len, and
 * the sender, become the peer's payload and address. */
int  np_psnr_p2p_filter(void* buf, uint32_t* len, uint8_t from_ip[4], uint16_t* from_port);

/* A peer's P2P endpoint became known (Matching2 member entries): punch toward
 * it a few times. Our own entry is skipped. */
void np_psnr_punch(uint32_t user, const uint8_t ip[4], uint16_t port);

/* The relay (psnr docs/api.md, "Relay"), when psnr runs with -relay.
 * - A peer that never answered our punches gets our datagrams through the
 *   relay: np_psnr_p2p_route sends them there and returns 1; 0 means send
 *   directly as usual.
 * - psnr says (ROUTE) which peers take streams only through the relay:
 *   np_psnr_stream_connect opens one to such a peer and returns its socket,
 *   -2 when the peer takes direct connections, -1 when the relay failed.
 * - A stream a peer opened to us arrives as a socket, with the peer's
 *   address, through the sink sysNet registers; sysNet hands it to the
 *   title's P2P listener. */
int     np_psnr_p2p_route(const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port);
int64_t np_psnr_stream_connect(const uint8_t ip[4], uint16_t port);
typedef void (*np_psnr_stream_sink_fn)(int64_t fd, const uint8_t ip[4], uint16_t port);
void    np_psnr_set_stream_sink(np_psnr_stream_sink_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_NP_PSNR_H */

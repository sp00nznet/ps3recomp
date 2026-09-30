/*
 * psnr.h - client for the psnr server (docs/api.md), in C for ps3recomp's
 * sceNp modules to vendor.
 *
 * No threads. psnr_send() queues a request and returns its id; the reply and
 * any server pushes come back out of psnr_poll(), which never blocks. That is
 * the shape NP's async APIs already have: the HLE module sends when the title
 * starts a request, and drains psnr_poll() on a guest thread (the title's own
 * poll or callback-check call), so callbacks run where the title expects them.
 * psnr_call() is the blocking form, for setup and tests.
 *
 * psnr_poll() also sends the heartbeat, so a client that stops polling for
 * 90 seconds is dropped, which is the right outcome for a hung title.
 *
 * Not thread-safe: one thread per psnr_client at a time.
 */
#ifndef PSNR_H
#define PSNR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Message types; payloads in docs/api.md. */
enum {
    PSNR_HELLO = 0x01, PSNR_HEARTBEAT = 0x02,
    PSNR_CREATE_ROOM = 0x10, PSNR_SEARCH_ROOMS = 0x11, PSNR_JOIN_ROOM = 0x12,
    PSNR_LEAVE_ROOM = 0x13, PSNR_SET_ROOM_DATA = 0x14, PSNR_ROOM_MESSAGE = 0x15,
    PSNR_KICK_MEMBER = 0x16,
    PSNR_RECORD_SCORE = 0x20, PSNR_GET_RANKING = 0x21, PSNR_GET_RANKING_BY_ID = 0x22,

    PSNR_HELLO_ACK = 0x81, PSNR_ROOM_JOINED = 0x90, PSNR_ROOM_LIST = 0x91,
    PSNR_OK = 0x93, PSNR_SCORE_RECORDED = 0xB0, PSNR_RANKING = 0xB1,
    PSNR_ERROR = 0x8F,

    PSNR_MEMBER_JOINED = 0xA1, PSNR_MEMBER_LEFT = 0xA2, PSNR_ROOM_DATA = 0xA3,
    PSNR_ROOM_MSG = 0xA4, PSNR_KICKED = 0xA5,

    /* the relay (docs/api.md, "Relay") */
    PSNR_STREAM_CONNECT = 0x30, PSNR_STREAM_ACCEPT = 0x31, PSNR_STREAM_READY = 0x96,
    PSNR_STREAM_OFFER = 0xA6, PSNR_ROUTE = 0xA7
};
#define PSNR_ROUTE_STREAM_RELAY 1   /* ROUTE flags: streams to that player go through the relay */

/* ERROR codes */
enum {
    PSNR_E_BAD_REQUEST = 1, PSNR_E_NOT_FOUND = 2, PSNR_E_ROOM_FULL = 3,
    PSNR_E_NOT_OWNER = 4, PSNR_E_NOT_IN_ROOM = 5, PSNR_E_LIMIT = 6, PSNR_E_NO_HELLO = 7,
    PSNR_E_NAME_TAKEN = 8, PSNR_E_NO_RELAY = 9
};

typedef struct psnr_client psnr_client;

/* One reply or push. req is 0 for pushes. data excludes the request id and
 * is freed by psnr_msg_free. */
typedef struct {
    uint8_t  type;
    uint32_t req;
    uint8_t* data;
    uint32_t len;
} psnr_msg;

/* Connect and say HELLO. comm_id is the title's NP communication ID
 * ("NPWR00860_00"), online_id the player's name, p2p_port the UDP port the
 * title takes peer traffic on (0 if none). Fills the id the server assigned
 * and the address it sees this client at. NULL on failure. */
psnr_client* psnr_connect(const char* host, uint16_t port,
                          const char* comm_id, const char* online_id, uint16_t p2p_port,
                          uint32_t* out_user_id, uint8_t out_public_ip[4]);

/* NAT traversal (docs/api.md, "UDP"). The caller owns the P2P socket; these
 * build and read the packets it exchanges. Every one starts with "PSNR".
 *   - Send a PROBE from the P2P socket to psnr_server_addr, now and then (a
 *     router forgets an idle mapping), until a probe reply comes back. The
 *     server then hands other players this socket's public endpoint.
 *   - Send a few PUNCHes to a peer when it appears, so this side's router
 *     lets the peer's packets in.
 *   - Drop anything psnr_is_control() matches before the title sees it. */
#define PSNR_UDP_PROBE_LEN 17
#define PSNR_UDP_PUNCH_LEN 9
void psnr_server_addr(const psnr_client* c, uint8_t ip[4], uint16_t* port);
void psnr_probe_packet(const psnr_client* c, uint8_t out[PSNR_UDP_PROBE_LEN]);
void psnr_punch_packet(const psnr_client* c, uint8_t out[PSNR_UDP_PUNCH_LEN]);
int  psnr_is_control(const void* buf, size_t len);
/* 1 if buf is a probe reply; fills the public endpoint the server saw. */
int  psnr_probe_reply(const void* buf, size_t len, uint8_t ip[4], uint16_t* port);

/* The relay, for peers that can't reach each other directly. It is there only
 * if the server runs with -relay.
 *   Datagrams: wrap a payload for a peer and send it to psnr_server_addr from
 *   the P2P socket; what comes back from the server unwraps to the sender and
 *   the payload.
 *   Streams: when a ROUTE push says a peer takes streams only through the
 *   relay, psnr_stream_connect opens one to it; the peer gets a STREAM_OFFER
 *   push (stream_id u32 | from_user u32 | vport u16) and answers with
 *   psnr_stream_accept. Both return a connected, blocking socket carrying the
 *   stream's raw bytes (a SOCKET on Windows, an fd elsewhere), or -1. */
int     psnr_relay_available(const psnr_client* c);
size_t  psnr_relay_wrap(const psnr_client* c, uint32_t to_user, const void* payload, size_t len,
                        uint8_t* out, size_t cap);   /* 0 if it doesn't fit */
int     psnr_relay_unwrap(const void* buf, size_t len, uint32_t* from_user,
                          const uint8_t** payload, size_t* payload_len);
int64_t psnr_stream_connect(const psnr_client* c, uint32_t to_user, uint16_t vport, int timeout_ms);
int64_t psnr_stream_accept(const psnr_client* c, uint32_t stream_id, int timeout_ms);

/* After psnr_connect returned NULL: the ERROR code the server refused the
 * HELLO with (PSNR_E_NAME_TAKEN: another player on the server has that
 * name), or 0 if it never answered. */
int psnr_connect_error(void);
void psnr_close(psnr_client* c);

/* Queue a request. body excludes the request id. Returns the id, or 0 if
 * the connection is gone. */
uint32_t psnr_send(psnr_client* c, uint8_t type, const void* body, uint32_t len);

/* 1: *out holds the next reply or push. 0: nothing yet. -1: disconnected. */
int psnr_poll(psnr_client* c, psnr_msg* out);

/* Send and wait up to timeout_ms for that request's reply. Pushes and other
 * replies that arrive meanwhile stay queued for psnr_poll. 1 / 0 (timeout) / -1. */
int psnr_call(psnr_client* c, uint8_t type, const void* body, uint32_t len,
              psnr_msg* reply, int timeout_ms);

void psnr_msg_free(psnr_msg* m);

/* Big-endian field helpers for building and reading payloads. */
static inline uint8_t* psnr_put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return p + 2; }
static inline uint8_t* psnr_put32(uint8_t* p, uint32_t v) { psnr_put16(p, (uint16_t)(v >> 16)); return psnr_put16(p + 2, (uint16_t)v); }
static inline uint8_t* psnr_put64(uint8_t* p, uint64_t v) { psnr_put32(p, (uint32_t)(v >> 32)); return psnr_put32(p + 4, (uint32_t)v); }
static inline uint16_t psnr_get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t psnr_get32(const uint8_t* p) { return (uint32_t)psnr_get16(p) << 16 | psnr_get16(p + 2); }
static inline uint64_t psnr_get64(const uint8_t* p) { return (uint64_t)psnr_get32(p) << 32 | psnr_get32(p + 4); }

#ifdef __cplusplus
}
#endif

#endif /* PSNR_H */

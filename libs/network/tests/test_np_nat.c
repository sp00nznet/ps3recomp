/*
 * test_np_nat - np_psnr's NAT traversal and relay: probing psnr from the P2P
 * socket, consuming psnr's packets, punching toward peers, and moving a peer
 * that never answers to the relay. The socket is a fake sender that records
 * what goes out.
 *
 *   clang-cl /I include /Fe:test_np_nat.exe libs/network/tests/test_np_nat.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_np_nat libs/network/tests/test_np_nat.c
 */
#include "../psnr/psnr.c"
#include "../np_psnr.c"

#include <assert.h>
#ifdef _WIN32
#  define sleep_ms(n) Sleep(n)
#else
#  include <unistd.h>
#  define sleep_ms(n) usleep((n) * 1000)
#endif

const char* np_fake_username(void) { return "PS3Player"; }

static struct { uint8_t pkt[64]; uint32_t len; uint8_t ip[4]; uint16_t port; } s_sent[64];
static int s_nsent;

static int fake_send(const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port)
{
    memcpy(s_sent[s_nsent].pkt, buf, len < 64 ? len : 64);
    s_sent[s_nsent].len = len;
    memcpy(s_sent[s_nsent].ip, ip, 4);
    s_sent[s_nsent].port = port;
    s_nsent++;
    return (int)len;
}

static int count_to(const uint8_t ip[4], uint16_t port, uint8_t type)
{
    int n = 0;
    for (int i = 0; i < s_nsent; i++)
        if (!memcmp(s_sent[i].ip, ip, 4) && s_sent[i].port == port && s_sent[i].pkt[4] == type) n++;
    return n;
}

int main(void)
{
    /* A connected client, as psnr_connect would leave it, with the relay on. */
    psnr_client* c = (psnr_client*)calloc(1, sizeof(psnr_client));
    c->user_id = 7;
    c->token = 0x1234567;
    c->relay = 1;
    memcpy(c->server_ip, "\x0a\x63\x00\x0a", 4);   /* 10.99.0.10 */
    c->server_port = 36100;
    memcpy(c->local_ip, "\xc0\xa8\x0a\x0a", 4);    /* 192.168.10.10 */
    s_client = c;
    s_user_id = 7;
    const uint8_t* srv = c->server_ip;

    /* Nothing goes out until the title binds its P2P socket. */
    nat_tick();
    assert(s_nsent == 0);

    /* Then a probe to psnr's UDP port, carrying the token. */
    np_psnr_set_p2p_sender(fake_send);
    nat_tick();
    assert(s_nsent == 1 && s_sent[0].len == PSNR_UDP_PROBE_LEN && s_sent[0].port == 36100);
    assert(!memcmp(s_sent[0].ip, srv, 4) && !memcmp(s_sent[0].pkt, "PSNR\x01", 5));
    assert(psnr_get32(s_sent[0].pkt + 5) == 7 && psnr_get32(s_sent[0].pkt + 9) == 0x1234567);

    /* Unanswered, it repeats every half second. */
    nat_tick();
    assert(s_nsent == 1);
    sleep_ms(PROBE_MS + 50);
    nat_tick();
    assert(s_nsent == 2);

    /* The reply is psnr's: consumed, and probing drops to a refresh. */
    uint8_t buf[64], from[4] = { 10, 99, 0, 10 };
    uint16_t fport = 36100;
    uint32_t len = 11;
    memcpy(buf, "PSNR\x81\xcb\x00\x71\x07\x9c\x41", 11);
    assert(np_psnr_p2p_filter(buf, &len, from, &fport) == 1 && s_probed);
    sleep_ms(PROBE_MS + 50);
    nat_tick();
    assert(s_nsent == 2);

    /* Two peers appear. Five punches each, 200 ms apart; our own member
     * entry is skipped. */
    const uint8_t cone[4] = { 198, 51, 100, 9 }, sym[4] = { 203, 0, 113, 50 };
    np_psnr_punch(7, cone, 3658);
    np_psnr_punch(9, cone, 40001);
    np_psnr_punch(11, sym, 50000);
    for (int i = 0; i < 12; i++) {
        nat_tick();
        if (i == 2) {   /* the cone peer answers with a punch: direct works */
            uint16_t p = 40001;
            len = 9;
            memcpy(buf, "PSNR\x02\x00\x00\x00\x09", 9);
            assert(np_psnr_p2p_filter(buf, &len, (uint8_t*)cone, &p) == 1);
        }
        sleep_ms(PUNCH_MS / 2 + 10);
    }
    assert(count_to(cone, 40001, 0x02) == PUNCHES && count_to(sym, 50000, 0x02) == PUNCHES);
    assert(count_to(cone, 3658, 0x02) == 0);

    /* Title data from a peer passes through untouched. */
    uint16_t p = 40001;
    len = 4;
    memcpy(buf, "game", 4);
    assert(np_psnr_p2p_filter(buf, &len, (uint8_t*)cone, &p) == 0 && len == 4);

    /* Three seconds of silence from the other peer: its datagrams go through
     * the relay, the cone peer's stay direct. */
    assert(!np_psnr_p2p_route("x", 1, sym, 50000));
    sleep_ms(RELAY_AFTER);
    nat_tick();
    assert(np_psnr_p2p_route("hello", 5, sym, 50000) == 1);
    assert(!np_psnr_p2p_route("hello", 5, cone, 40001));
    int last = s_nsent - 1;
    assert(!memcmp(s_sent[last].ip, srv, 4) && s_sent[last].port == 36100 && s_sent[last].len == 17 + 5);
    assert(!memcmp(s_sent[last].pkt, "PSNR\x03", 5) && psnr_get32(s_sent[last].pkt + 13) == 11);
    assert(!memcmp(s_sent[last].pkt + 17, "hello", 5));

    /* What that peer sends back through the relay reaches the title as if it
     * came from the peer: payload in place, sender rewritten. */
    uint8_t got[4] = { 10, 99, 0, 10 };
    uint16_t gport = 36100;
    len = 9 + 3;
    memcpy(buf, "PSNR\x03\x00\x00\x00\x0b" "hey", 12);
    assert(np_psnr_p2p_filter(buf, &len, got, &gport) == 0);
    assert(len == 3 && !memcmp(buf, "hey", 3) && !memcmp(got, sym, 4) && gport == 50000);

    /* A relayed datagram from someone we don't know is dropped. */
    len = 12;
    memcpy(buf, "PSNR\x03\x00\x00\x00\x63" "who", 12);
    assert(np_psnr_p2p_filter(buf, &len, got, &gport) == 1);

    /* ROUTE: streams to user 11 go through the relay; to 9 they don't. */
    uint8_t route[13] = { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 11, 1 };
    psnr_msg m = { PSNR_ROUTE, 0, route, sizeof(route) };
    assert(nat_push(&m) == 1);
    assert(peer_by_user(11)->relay_stream && !peer_by_user(9)->relay_stream);
    assert(np_psnr_stream_connect(cone, 40001) == -2);   /* direct, as usual */

    /* The socket closes: nothing more goes out. */
    np_psnr_set_p2p_sender(NULL);
    int before = s_nsent;
    np_psnr_punch(13, cone, 40002);
    sleep_ms(PUNCH_MS + 10);
    nat_tick();
    assert(s_nsent == before);

    printf("test_np_nat: all passed\n");
    return 0;
}

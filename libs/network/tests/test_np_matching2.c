/*
 * test_np_matching2 - Matching2's request/response plumbing without a server.
 *
 * Compiles sceNpMatching2.c into this file with psnr and sysutil stubbed: the
 * stubs capture what would go to the server and what would be queued as a
 * guest callback. Checks the three things that went wrong while bringing up
 * Simpsons Arcade or could silently: GetEventData relocating the pointers in
 * a response into the title's buffer, attributes packed for psnr and unpacked
 * back, and the RoomDataInternal / member structs built from a ROOM_JOINED.
 *
 *   clang-cl /I include /Fe:test_np_matching2.exe libs/network/tests/test_np_matching2.c libs/network/psnr/psnr.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_np_matching2 libs/network/tests/test_np_matching2.c libs/network/psnr/psnr.c
 */

#include "../sceNpMatching2.c"

#include <assert.h>

uint8_t* vm_base;
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t addr) { (void)addr; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width) { (void)addr; (void)val; (void)width; }

/* sceNp */
s32 sceNpInit(u32 poolSize, void* poolPtr) { (void)poolSize; (void)poolPtr; return 0; }
s32 sceNpTerm(void) { return 0; }

/* psnr: always connected; requests are captured, not sent */
static np_psnr_reply_fn s_fn;
static void* s_user;
static u8 s_body[4096];
static u32 s_len;
static uint8_t s_type;
static void (*s_push)(const psnr_msg*);
int np_psnr_enabled(void) { return 1; }
int np_psnr_connect(const char* comm_id) { (void)comm_id; return 0; }
int np_psnr_connected(void) { return 1; }
uint32_t np_psnr_request(uint8_t type, const void* body, uint32_t len, np_psnr_reply_fn fn, void* user)
{
    s_type = type; s_len = len; memcpy(s_body, body, len); s_fn = fn; s_user = user;
    return 1;
}
void np_psnr_on_push(void (*fn)(const psnr_msg*)) { s_push = fn; }
void np_psnr_on_tick(void (*fn)(void)) { (void)fn; }
void np_psnr_punch(uint32_t user, const uint8_t ip[4], uint16_t port) { (void)user; (void)ip; (void)port; }

/* sysutil: remember the last callback queued */
static u32 s_cb_opd;
static u64 s_cb[8];
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8])
{
    s_cb_opd = opd;
    memcpy(s_cb, args, sizeof(s_cb));
    return 0;
}

enum { CTXID = 0x100, COMM = 0x110, OPT = 0x120, REQID = 0x130, REQ = 0x200,
       ATTRS = 0x400, BIN = 0x500, BUF = 0x10000 };

/* Deliver the captured request's reply, built by the test. */
static void reply(uint8_t type, const u8* data, u32 len)
{
    psnr_msg m = { type, 1, (u8*)data, len };
    s_fn(s_user, &m);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 1u << 20);

    assert(sceNpMatching2Init2(0, 0, 0) == 0);
    memcpy(vm_base + COMM, "NPWR01444", 9);
    assert(sceNpMatching2CreateContext(0, COMM, 0, CTXID, 0) == 0);
    u16 ctx = vm_read16(CTXID);
    vm_write32(OPT, 0xCB00);      /* request callback opd */
    vm_write32(OPT + 4, 0xA0);    /* its arg */
    assert(sceNpMatching2ContextStartAsync(ctx, 0) == 0);
    assert(s_ctx[ctx].started && strcmp(s_ctx[ctx].comm_id, "NPWR01444_00") == 0);

    /* GetWorldInfoList: { world*, worldNum } then the world, relocated into BUF. */
    assert(sceNpMatching2GetWorldInfoList(ctx, REQ, OPT, REQID) == 0);
    assert(s_cb_opd == 0xCB00 && s_cb[2] == 0x0002 && s_cb[6] == 0xA0);
    u32 key = (u32)s_cb[3], size = (u32)s_cb[5];
    assert(sceNpMatching2GetEventData(ctx, key, BUF, 4096) == (s32)size);
    assert(vm_read32(BUF) == BUF + 8 && vm_read32(BUF + 4) == 1);   /* pointer lands in BUF */
    assert(vm_read32(vm_read32(BUF)) == 1);                        /* worldId */

    /* CreateJoinRoom: one searchable int attr, one member bin attr. */
    memset(vm_base + REQ, 0, 112);
    vm_write32(REQ + 16, 4);                       /* maxSlot */
    vm_write32(REQ + 20, 0x80000000u);             /* flagAttr */
    vm_write16(ATTRS, 0x4C); vm_write32(ATTRS + 4, 7);
    vm_write32(REQ + 32, ATTRS); vm_write32(REQ + 36, 1);
    vm_write16(ATTRS + 16, 0x59); vm_write32(ATTRS + 20, BIN); vm_write32(ATTRS + 24, 3);
    memcpy(vm_base + BIN, "abc", 3);
    vm_write32(REQ + 92, ATTRS + 16); vm_write32(REQ + 96, 1);
    assert(sceNpMatching2CreateJoinRoom(ctx, REQ, OPT, REQID) == 0);
    assert(s_type == PSNR_CREATE_ROOM && s_body[0] == 4 && psnr_get32(s_body + 1) == 0x80000000u);

    m2_attrs ext, mine;
    u16 el = psnr_get16(s_body + 5);
    attrs_unpack(&ext, s_body + 7, el);
    assert(ext.n == 1 && ext.a[0].id == 0x4C && ext.a[0].kind == 0 && ext.a[0].num == 7);
    const u8* p = s_body + 7 + el;
    p += 2 + psnr_get16(p);                         /* internal: empty */
    attrs_unpack(&mine, p + 2, psnr_get16(p));
    assert(mine.n == 1 && mine.a[0].id == 0x59 && mine.a[0].len == 3 && !memcmp(mine.a[0].bin, "abc", 3));

    /* The server answers: room 9, me = member 1 = owner, max 4, one member. */
    u8 r[128], *w = r;
    w = psnr_put64(w, 9); w = psnr_put16(w, 1); w = psnr_put16(w, 1); *w++ = 4;
    w = psnr_put32(w, 0x80000000u);
    w = psnr_put16(w, 0);                          /* external */
    w = psnr_put16(w, 0);                          /* internal */
    *w++ = 1;                                      /* members */
    w = psnr_put16(w, 1); w = psnr_put32(w, 42);
    memset(w, 0, 16); memcpy(w, "homer", 5); w += 16;
    *w++ = 127; *w++ = 0; *w++ = 0; *w++ = 1; w = psnr_put16(w, 3658); *w++ = 1;
    u8 packed[32];
    u32 pl = attrs_pack(&mine, packed);
    w = psnr_put16(w, (u16)pl); memcpy(w, packed, pl); w += pl;
    reply(PSNR_ROOM_JOINED, r, (u32)(w - r));

    assert(s_cb[2] == 0x0101 && s_cb[4] == 0);    /* CreateJoinRoom, no error */
    key = (u32)s_cb[3];
    assert(sceNpMatching2GetEventData(ctx, key, BUF, 4096) > 0);
    u32 rd = vm_read32(BUF);                       /* roomDataInternal */
    assert(rd > BUF && vm_read64(rd + 16) == 9 && vm_read32(rd + 32) == 4);
    assert(vm_read32(rd + 40) == 1);               /* membersNum */
    u32 mem = vm_read32(rd + 36);
    assert(mem && vm_read32(rd + 44) == mem && vm_read32(rd + 48) == mem);   /* me, owner */
    assert(vm_read16(mem + 56) == 1);              /* memberId */
    assert(!memcmp(vm_base + mem + 4, "homer", 5));  /* userInfo.npId */
    assert(vm_read32(mem + 60) == MEMBER_FLAG_OWNER);
    assert(vm_read32(mem + 80) == 1);              /* the member's bin attr came back */
    u32 ba = vm_read32(mem + 76);
    assert(vm_read16(ba) == 0x59 && vm_read32(ba + 8) == 3 && !memcmp(vm_base + vm_read32(ba + 4), "abc", 3));

    /* A second member arrives: a room event, then signaling says it's reachable. */
    w = r;
    w = psnr_put64(w, 9);
    w = psnr_put16(w, 2); w = psnr_put32(w, 43);
    memset(w, 0, 16); memcpy(w, "bart", 4); w += 16;
    *w++ = 127; *w++ = 0; *w++ = 0; *w++ = 1; w = psnr_put16(w, 3659); *w++ = 0;
    w = psnr_put16(w, 0);
    s_ctx[ctx].sig_cb = 0x5100; s_ctx[ctx].room_cb = 0x1100;
    psnr_msg push = { PSNR_MEMBER_JOINED, 0, r, (u32)(w - r) };
    s_push(&push);
    assert(s_cb_opd == 0x1100 && s_cb[2] == 0x1101);                  /* MemberJoined now... */
    m2_tick();                                                         /* ...established later */
    assert(s_cb_opd == 0x1100);
    for (s_sig[0].due = 0; s_cb_opd != 0x5100; ) m2_tick();
    assert(s_cb_opd == 0x5100 && s_cb[2] == 2 && s_cb[3] == 0x5102);   /* Established for member 2 */
    vm_write32(REQ, 0);
    assert(sceNpMatching2SignalingGetConnectionStatus(ctx, 9, 2, REQ, REQ + 4, REQ + 8) == 0);
    assert(vm_read32(REQ) == 2 && vm_read8(REQ + 4) == 127 && vm_read16(REQ + 8) == 3659);

    printf("test_np_matching2: all passed\n");
    return 0;
}

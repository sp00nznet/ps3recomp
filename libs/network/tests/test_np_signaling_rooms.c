/*
 * test_np_signaling_rooms - sceNpSignaling connections for Matching2 room members.
 *
 *   cc -std=gnu17 -I include -o test_np_signaling_rooms libs/network/tests/test_np_signaling_rooms.c
 *
 * Compiles sceNpSignaling.c into this file with sysutil stubbed: the stub
 * records what would be queued for the title's handlers. Checks that context
 * ids are not 0 (titles read 0 as "no context"), that ActivateConnection gives a
 * connection that stays PENDING until the room member is up, that "up" reaches
 * the handler and the extended handler as ESTABLISHED and the extended handler
 * also as EXT_PEER_ACTIVATED then EXT_MUTUAL_ACTIVATED, with the SDK values, that
 * the status, peer address, peer NP ID and FromNpId answers come from the member
 * record, and that "down" is DEAD.
 */
#include "../sceNpSignaling.c"

#include <assert.h>

uint8_t* vm_base;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_linelock_lock(uint32_t a) { (void)a; }   /* ppu_memory.h's per-line lock, where it has one */
void spu_linelock_release(uint32_t a) { (void)a; }

int np_is_initialized(void) { return 1; }   /* sceNpInit has run */

#define QMAX 16
static struct { u32 opd; u64 a[8]; } s_q[QMAX];
static int s_nq;
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8])
{
    assert(s_nq < QMAX);
    s_q[s_nq].opd = opd;
    memcpy(s_q[s_nq].a, args, sizeof(s_q[s_nq].a));
    s_nq++;
    return CELL_OK;
}

static u32 be32_at(u32 ea)
{
    const uint8_t* p = vm_base + ea;
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
static u32 be16_at(u32 ea) { return (u32)vm_base[ea] << 8 | vm_base[ea + 1]; }

/* The queued call k: handler opd, (ctx, conn, event, error, arg). */
static void expect(int k, u32 opd, u32 ctx, u32 conn, u32 event, u32 arg)
{
    assert(k < s_nq);
    assert(s_q[k].opd == opd);
    assert(s_q[k].a[0] == ctx && s_q[k].a[1] == conn && s_q[k].a[2] == event && s_q[k].a[3] == 0 && s_q[k].a[4] == arg);
}

#define EA(x) ((void*)(uintptr_t)(x))

int main(void)
{
    static uint8_t mem[0x10000];
    vm_base = mem;
    const u32 H = 0x8000, HARG = 0x8100, X = 0x8200, XARG = 0x8300;   /* handler and extended handler descriptors */
    const u32 ctx_ea = 0x100, conn_ea = 0x104, st_ea = 0x108, addr_ea = 0x10C, port_ea = 0x110, info_ea = 0x200,
              npid_ea = 0x300;

    /* Context ids start at 1. */
    assert(sceNpSignalingCreateCtx(NULL, (SceNpSignalingHandler)EA(H), EA(HARG), (SceNpSignalingCtxId*)EA(ctx_ea)) == CELL_OK);
    const u32 ctx = be32_at(ctx_ea);
    assert(ctx != 0);
    assert(sceNpSignalingAddExtendedHandler(ctx, (SceNpSignalingHandler)EA(X), EA(XARG)) == CELL_OK);
    assert(sceNpSignalingAddExtendedHandler(0, (SceNpSignalingHandler)EA(X), EA(XARG)) == (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);

    /* The title activates its connection to a room member first: pending, no event yet. */
    memset(mem + npid_ea, 0, 36);
    memcpy(mem + npid_ea, "peer-2", 6);
    assert(sceNpSignalingActivateConnection(ctx, EA(npid_ea), (SceNpSignalingConnId*)EA(conn_ea)) == CELL_OK);
    const u32 conn = be32_at(conn_ea);
    assert(conn != 0);
    assert(sceNpSignalingGetConnectionStatus(ctx, conn, (s32*)EA(st_ea), NULL, NULL) == CELL_OK);
    assert(be32_at(st_ea) == SCE_NP_SIGNALING_CONN_STATUS_PENDING);
    assert(s_nq == 0);

    /* Matching2 says the member is up. */
    const u8 ip[4] = { 192, 168, 1, 20 };
    np_signaling_room_peer("peer-2", ip, 3659, 1);
    assert(s_nq == 4);
    expect(0, H, ctx, conn, 1, HARG);    /* SCE_NP_SIGNALING_EVENT_ESTABLISHED */
    expect(1, X, ctx, conn, 1, XARG);    /* the extended handler gets the normal events too */
    expect(2, X, ctx, conn, 10, XARG);   /* EXT_PEER_ACTIVATED, extended handler only */
    expect(3, X, ctx, conn, 12, XARG);   /* EXT_MUTUAL_ACTIVATED */
    np_signaling_room_peer("peer-2", ip, 3659, 1);   /* no change, no event */
    assert(s_nq == 4);

    assert(sceNpSignalingGetConnectionStatus(ctx, conn, (s32*)EA(st_ea), (u32*)EA(addr_ea), (u16*)EA(port_ea)) == CELL_OK);
    assert(be32_at(st_ea) == SCE_NP_SIGNALING_CONN_STATUS_ACTIVE);
    assert(be32_at(addr_ea) == 0xC0A80114u && be16_at(port_ea) == 3659);
    assert(sceNpSignalingGetConnectionInfo(ctx, conn, SCE_NP_SIGNALING_CONN_INFO_PEER_ADDRESS, EA(info_ea)) == CELL_OK);
    assert(be32_at(info_ea) == 0xC0A80114u && be16_at(info_ea + 4) == 3659);
    assert(sceNpSignalingGetConnectionInfo(ctx, conn, SCE_NP_SIGNALING_CONN_INFO_PEER_NPID, EA(info_ea)) == CELL_OK);
    assert(memcmp(mem + info_ea, "peer-2", 7) == 0);
    assert(sceNpSignalingGetConnectionInfo(ctx, conn, 99, EA(info_ea)) == (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    assert(sceNpSignalingGetPeerNetInfo(ctx, conn, (u32*)EA(addr_ea), (u16*)EA(port_ea)) == CELL_OK);
    assert(be32_at(addr_ea) == 0xC0A80114u && be16_at(port_ea) == 3659);
    vm_write32(conn_ea, 0);
    assert(sceNpSignalingGetConnectionFromNpId(ctx, EA(npid_ea), (SceNpSignalingConnId*)EA(conn_ea)) == CELL_OK);
    assert(be32_at(conn_ea) == conn);
    assert(sceNpSignalingGetConnectionStatus(ctx, conn + 1, (s32*)EA(st_ea), NULL, NULL) == (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND);

    /* A member that comes up before the title activates it: ESTABLISHED now, mutual once activated. */
    const u8 ip3[4] = { 192, 168, 1, 30 };
    np_signaling_room_peer("peer-3", ip3, 3660, 1);
    assert(s_nq == 6);
    memcpy(mem + npid_ea, "peer-3", 6);
    assert(sceNpSignalingActivateConnection(ctx, EA(npid_ea), (SceNpSignalingConnId*)EA(conn_ea)) == CELL_OK);
    const u32 conn3 = be32_at(conn_ea);
    assert(conn3 != conn);
    expect(4, H, ctx, conn3, 1, HARG);
    expect(5, X, ctx, conn3, 1, XARG);
    expect(s_nq - 2, X, ctx, conn3, 10, XARG);
    expect(s_nq - 1, X, ctx, conn3, 12, XARG);
    const int before_down = s_nq;

    /* The member leaves: DEAD (0) to both handlers. */
    np_signaling_room_peer("peer-2", ip, 3659, 0);
    assert(s_nq == before_down + 2);
    expect(before_down, H, ctx, conn, 0, HARG);
    expect(before_down + 1, X, ctx, conn, 0, XARG);
    assert(sceNpSignalingGetConnectionStatus(ctx, conn, (s32*)EA(st_ea), NULL, NULL) == CELL_OK);
    assert(be32_at(st_ea) == SCE_NP_SIGNALING_CONN_STATUS_INACTIVE);

    assert(sceNpSignalingDestroyCtx(ctx) == CELL_OK);
    assert(sceNpSignalingDestroyCtx(ctx) == (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    printf("test_np_signaling_rooms: ok (ctx %u, conns %u and %u, %d events)\n", ctx, conn, conn3, s_nq);
    return 0;
}

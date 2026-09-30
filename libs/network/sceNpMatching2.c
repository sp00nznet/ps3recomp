/*
 * ps3recomp - sceNpMatching2 over psnr
 *
 * A title's rooms live on a psnr server (np_psnr.h); game traffic goes peer
 * to peer over sys_net P2P sockets, to the address and port psnr gives each
 * member. Offline (no PSNR_SERVER) every server request returns
 * SERVER_NOT_AVAILABLE, the old behaviour.
 *
 * How the SDK's asynchrony is kept:
 * - A request returns at once with a request id. Its answer comes back as a
 *   request callback (ctxId, reqId, event, eventKey, errorCode, dataSize,
 *   arg), queued through cellSysutilQueueGuestCallbackArgs and delivered on
 *   the title's next cellSysutilCheckCallback. The psnr pump runs from there
 *   too, so the whole round trip happens on the title's polling thread.
 * - Response data is fetched with GetEventData(eventKey, buf). SDK responses
 *   contain pointers into themselves, so each one is built as a relocatable
 *   image (rb_t): offsets from its start, plus a list of the pointer fields.
 *   GetEventData copies the image into the title's buffer and adds the
 *   buffer's address to each pointer field.
 * - Room pushes from psnr become room-event, room-message and signaling
 *   callbacks. Signaling is "established" as soon as psnr names a member: the
 *   peer's address and P2P port are all the title's own P2P sockets need.
 *
 * Structure layouts are the SDK ABI. The request layouts, RoomDataExternal,
 * RoomDataInternal's roomId and every callback signature were checked
 * against Simpsons Arcade (NPUB30563) reading and writing them.
 *
 * Attributes travel to psnr as opaque blobs, packed here:
 * entries of  id u16 | kind u8 (0 int, 1 bin) | int: u32 / bin: u16 len + bytes.
 * Room external = searchable int + searchable bin + bin external attrs;
 * room internal = bin internal attrs; member data = member bin internal attrs.
 * Search filtering (flags, int and bin filters) happens here, on the rooms
 * psnr lists.
 */

#include "sceNpMatching2.h"
#include "sceNp.h"
#include "np_psnr.h"
#include "../system/cellSysutil.h"
#include "../../runtime/ppu/ppu_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#  define LOCK()   AcquireSRWLockExclusive(&s_lock)
#  define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#  include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#  define LOCK()   pthread_mutex_lock(&s_lock)
#  define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

/* ---------------------------------------------------------------------------
 * SDK constants
 * -----------------------------------------------------------------------*/

enum {
    EV_GetServerInfo = 0x0001, EV_GetWorldInfoList = 0x0002, EV_SetRoomDataExternal = 0x0004,
    EV_CreateServerContext = 0x0009, EV_DeleteServerContext = 0x000A,
    EV_CreateJoinRoom = 0x0101, EV_JoinRoom = 0x0102, EV_LeaveRoom = 0x0103,
    EV_KickoutRoomMember = 0x0105, EV_SearchRoom = 0x0106, EV_SendRoomMessage = 0x0108,
    EV_SetRoomDataInternal = 0x0109, EV_SignalingGetPingInfo = 0x0E01,

    ROOM_EV_MemberJoined = 0x1101, ROOM_EV_MemberLeft = 0x1102, ROOM_EV_Kickedout = 0x1103,
    ROOM_EV_RoomOwnerChanged = 0x1105, ROOM_EV_UpdatedRoomDataInternal = 0x1106,
    ROOM_MSG_EV_Message = 0x2102,
    SIG_EV_Dead = 0x5101, SIG_EV_Established = 0x5102,
    CTX_EV_StartOver = 0x6F01, CTX_EV_Start = 0x6F02, CTX_EV_Stop = 0x6F03,

    CAUSE_LEAVE_ACTION = 1, CAUSE_KICKOUT_ACTION = 2, CAUSE_MEMBER_DISAPPEARED = 5,
    CAUSE_CONTEXT_ERROR = 10, CAUSE_CONTEXT_ACTION = 11,

    CAST_UNICAST = 1, CAST_MULTICAST = 2, CAST_BROADCAST = 4,
    SIG_CONN_INACTIVE = 0, SIG_CONN_ACTIVE = 2,
    SERVER_STATUS_AVAILABLE = 1,
};

#define ROOM_FLAG_FULL          0x20000000u
#define MEMBER_FLAG_OWNER       0x80000000u
#define SERVER_ID               1
#define WORLD_ID                1

/* ---------------------------------------------------------------------------
 * Relocatable response images
 * -----------------------------------------------------------------------*/

typedef struct {
    u8*  b;
    u32  len, cap;
    u32* rel;       /* offsets of pointer fields */
    u32  nrel, relcap;
} rb_t;

static u32 rb_alloc(rb_t* r, u32 size, u32 align)
{
    u32 off = (r->len + align - 1) & ~(align - 1);
    if (off + size > r->cap) {
        while (off + size > r->cap) r->cap = r->cap ? r->cap * 2 : 1024;
        r->b = (u8*)realloc(r->b, r->cap);
    }
    memset(r->b + r->len, 0, off + size - r->len);
    r->len = off + size;
    return off;
}
static void rb_8(rb_t* r, u32 o, u8 v)   { r->b[o] = v; }
static void rb_16(rb_t* r, u32 o, u16 v) { r->b[o] = (u8)(v >> 8); r->b[o + 1] = (u8)v; }
static void rb_32(rb_t* r, u32 o, u32 v) { rb_16(r, o, (u16)(v >> 16)); rb_16(r, o + 2, (u16)v); }
static void rb_64(rb_t* r, u32 o, u64 v) { rb_32(r, o, (u32)(v >> 32)); rb_32(r, o + 4, (u32)v); }
static void rb_ptr(rb_t* r, u32 o, u32 target)
{
    if (r->nrel == r->relcap) {
        r->relcap = r->relcap ? r->relcap * 2 : 32;
        r->rel = (u32*)realloc(r->rel, r->relcap * sizeof(u32));
    }
    r->rel[r->nrel++] = o;
    rb_32(r, o, target);
}
static void rb_free(rb_t* r) { free(r->b); free(r->rel); memset(r, 0, sizeof(*r)); }

/* ---------------------------------------------------------------------------
 * State
 * -----------------------------------------------------------------------*/

#define ATTR_MAX    16
#define BIN_MAX     256
#define MEMBER_MAX  16

typedef struct { u16 id; u8 kind; u32 num; u16 len; u8 bin[BIN_MAX]; } m2_attr;
typedef struct { m2_attr a[ATTR_MAX]; int n; } m2_attrs;

typedef struct {
    u16 id;
    u32 user;
    char online_id[17];
    u8  ip[4];
    u16 port;
    u64 joined;
    m2_attrs data;  /* member bin attrs internal */
} m2_member;

typedef struct {
    int in;
    u64 id;
    u16 me, owner;
    u8  max;
    u32 flags;
    m2_attrs ext, in_attrs;
    m2_member m[MEMBER_MAX];
    int n;
} m2_room;

typedef struct {
    int in_use, started;
    char comm_id[16];
    u32 ctx_cb, ctx_arg;
    u32 room_cb, room_arg;
    u32 msg_cb, msg_arg;
    u32 sig_cb, sig_arg;
    u32 def_cb, def_arg;   /* default request opt param */
    m2_room room;
} m2_ctx;

static int    s_init;
static m2_ctx s_ctx[SCE_NP_MATCHING2_CTX_MAX + 1];   /* ids are 1-based */
static u32    s_next_req = 1;
static u32    s_next_key = 1;

#define EVENT_RING 64
static struct { u32 key; rb_t img; } s_events[EVENT_RING];

static m2_ctx* ctx_get(u16 id)
{
    return (id >= 1 && id <= SCE_NP_MATCHING2_CTX_MAX && s_ctx[id].in_use) ? &s_ctx[id] : NULL;
}

static u64 now_usec(void)
{
    return (u64)time(NULL) * 1000000ull;
}

/* ---------------------------------------------------------------------------
 * Callbacks into the title
 * -----------------------------------------------------------------------*/

static void queue_cb(u32 opd, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7)
{
    const u64 args[8] = { a0, a1, a2, a3, a4, a5, a6, a7 };
    if (opd) cellSysutilQueueGuestCallbackArgs(opd, args);
}

/* Keep an image for GetEventData; returns its key. Takes ownership. */
static u32 store_event(rb_t* img)
{
    LOCK();
    u32 key = s_next_key++;
    int slot = (int)(key % EVENT_RING);
    rb_free(&s_events[slot].img);
    s_events[slot].key = key;
    s_events[slot].img = *img;
    UNLOCK();
    memset(img, 0, sizeof(*img));
    return key;
}

/* A request in flight: who to answer and how. */
typedef struct {
    u16 ctx;
    u32 req;
    u16 event;
    u32 cb, arg;
    /* SearchRoom */
    u32 start, max, flag_filter, flag_attr;
    struct { u8 op; u16 id; u32 num; } ifl[8];
    int nifl;
    u16 attr_ids[16];
    int nattr;
    /* SetRoomDataInternal: what changed, for the owner's own update event */
    u32 prev_flags;
    u8  flags_changed, bins_changed;
} m2_op;

static void answer(const m2_op* op, s32 err, rb_t* img)
{
    u32 key = 0, size = 0;
    if (img && img->len) {
        size = img->len;
        key = store_event(img);
    }
    printf("[sceNpMatching2] -> request %u event 0x%04X err 0x%08X, %u bytes%s\n",
           op->req, op->event, (u32)err, size, op->cb ? "" : " (no callback)");
    queue_cb(op->cb, op->ctx, op->req, op->event, key, (u64)(u32)err, size, op->arg, 0);
}

/* Resolve a request's opt param (or the context default) and assign an id. */
static m2_op* op_new(m2_ctx* c, u16 ctx, u16 event, u32 opt, u32 req_id_ea)
{
    m2_op* op = (m2_op*)calloc(1, sizeof(m2_op));
    op->ctx = ctx;
    op->event = event;
    op->cb = opt ? vm_read32(opt) : c->def_cb;
    op->arg = opt ? vm_read32(opt + 4) : c->def_arg;
    LOCK();
    op->req = s_next_req++;
    UNLOCK();
    if (req_id_ea) vm_write32(req_id_ea, op->req);
    printf("[sceNpMatching2] request %u: event 0x%04X\n", op->req, event);
    return op;
}

/* Answer at once (no server round trip) and free the op. */
static s32 answer_now(m2_op* op, s32 err, rb_t* img)
{
    answer(op, err, img);
    free(op);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Attributes: guest arrays <-> m2_attrs <-> packed blobs
 * -----------------------------------------------------------------------*/

/* SceNpMatching2IntAttr { u16 id; u8 pad[2]; u32 num; }                    8 bytes */
static void attrs_read_int(m2_attrs* out, u32 arr, u32 n)
{
    for (u32 i = 0; i < n && arr; i++) {
        u16 id = vm_read16(arr + i * 8);
        m2_attr* a = NULL;
        for (int k = 0; k < out->n; k++) if (out->a[k].id == id) a = &out->a[k];
        if (!a && out->n < ATTR_MAX) a = &out->a[out->n++];
        if (!a) continue;
        a->id = id; a->kind = 0; a->num = vm_read32(arr + i * 8 + 4);
    }
}

/* SceNpMatching2BinAttr { u16 id; u8 pad[2]; u32 ptr; u32 size; }           12 bytes */
static void attrs_read_bin(m2_attrs* out, u32 arr, u32 n)
{
    for (u32 i = 0; i < n && arr; i++) {
        u16 id = vm_read16(arr + i * 12);
        u32 p = vm_read32(arr + i * 12 + 4), size = vm_read32(arr + i * 12 + 8);
        m2_attr* a = NULL;
        for (int k = 0; k < out->n; k++) if (out->a[k].id == id) a = &out->a[k];
        if (!a && out->n < ATTR_MAX) a = &out->a[out->n++];
        if (!a) continue;
        if (size > BIN_MAX) size = BIN_MAX;
        a->id = id; a->kind = 1; a->len = (u16)size;
        if (p && size) memcpy(a->bin, vm_ptr8(p), size);
    }
}

static u32 attrs_pack(const m2_attrs* in, u8* out)
{
    u8* p = out;
    for (int i = 0; i < in->n; i++) {
        const m2_attr* a = &in->a[i];
        p = psnr_put16(p, a->id);
        *p++ = a->kind;
        if (a->kind == 0) p = psnr_put32(p, a->num);
        else { p = psnr_put16(p, a->len); memcpy(p, a->bin, a->len); p += a->len; }
    }
    return (u32)(p - out);
}

static void attrs_unpack(m2_attrs* out, const u8* p, u32 len)
{
    const u8* end = p + len;
    out->n = 0;
    while (p + 3 <= end && out->n < ATTR_MAX) {
        m2_attr* a = &out->a[out->n];
        a->id = psnr_get16(p);
        a->kind = p[2];
        p += 3;
        if (a->kind == 0) {
            if (p + 4 > end) break;
            a->num = psnr_get32(p); p += 4;
        } else {
            if (p + 2 > end) break;
            a->len = psnr_get16(p); p += 2;
            if (a->len > BIN_MAX || p + a->len > end) break;
            memcpy(a->bin, p, a->len); p += a->len;
        }
        out->n++;
    }
}

static const m2_attr* attr_find(const m2_attrs* s, u16 id)
{
    for (int i = 0; i < s->n; i++) if (s->a[i].id == id) return &s->a[i];
    return NULL;
}

/* Write attrs of one kind as a guest array; returns (offset, count). */
static u32 rb_attrs(rb_t* r, const m2_attrs* s, u8 kind, u32* count)
{
    u32 n = 0;
    for (int i = 0; i < s->n; i++) if (s->a[i].kind == kind) n++;
    *count = n;
    if (!n) return 0;
    u32 arr = rb_alloc(r, n * (kind ? 12 : 8), 4);
    u32 k = 0;
    for (int i = 0; i < s->n; i++) {
        const m2_attr* a = &s->a[i];
        if (a->kind != kind) continue;
        if (kind == 0) {
            rb_16(r, arr + k * 8, a->id);
            rb_32(r, arr + k * 8 + 4, a->num);
        } else {
            u32 data = rb_alloc(r, a->len ? a->len : 1, 4);
            memcpy(r->b + data, a->bin, a->len);
            rb_16(r, arr + k * 12, a->id);
            rb_ptr(r, arr + k * 12 + 4, data);
            rb_32(r, arr + k * 12 + 8, a->len);
        }
        k++;
    }
    return arr;
}

/* ---------------------------------------------------------------------------
 * Room state from psnr messages
 * -----------------------------------------------------------------------*/

/* member entry: id u16 | user u32 | online_id [16] | ip [4] | port u16 | owner u8 | data blob */
static const u8* parse_member(m2_member* m, const u8* p, const u8* end, int* owner)
{
    if (p + 31 > end) return NULL;
    memset(m, 0, sizeof(*m));
    m->id = psnr_get16(p);
    m->user = psnr_get32(p + 2);
    memcpy(m->online_id, p + 6, 16);
    memcpy(m->ip, p + 22, 4);
    m->port = psnr_get16(p + 26);
    np_psnr_punch(m->user, m->ip, m->port);   /* open our router toward them */
    *owner = p[28];
    u16 dl = psnr_get16(p + 29);
    p += 31;
    if (p + dl > end) return NULL;
    attrs_unpack(&m->data, p, dl);
    m->joined = now_usec();
    return p + dl;
}

/* ROOM_JOINED: room u64 | me u16 | owner u16 | max u8 | flags u32 | ext blob | int blob | n u8 | members */
static int parse_joined(m2_room* r, const psnr_msg* m)
{
    const u8 *p = m->data, *end = m->data + m->len;
    if (m->len < 17) return -1;
    memset(r, 0, sizeof(*r));
    r->id = psnr_get64(p);
    r->me = psnr_get16(p + 8);
    r->owner = psnr_get16(p + 10);
    r->max = p[12];
    r->flags = psnr_get32(p + 13);
    p += 17;
    u16 l = psnr_get16(p); p += 2;
    if (p + l > end) return -1;
    attrs_unpack(&r->ext, p, l); p += l;
    l = psnr_get16(p); p += 2;
    if (p + l > end) return -1;
    attrs_unpack(&r->in_attrs, p, l); p += l;
    if (p >= end) return -1;
    int n = *p++, owner;
    for (int i = 0; i < n && i < MEMBER_MAX; i++) {
        if (!(p = parse_member(&r->m[i], p, end, &owner))) return -1;
        r->n++;
    }
    r->in = 1;
    return 0;
}

static m2_member* room_member(m2_room* r, u16 id)
{
    for (int i = 0; i < r->n; i++) if (r->m[i].id == id) return &r->m[i];
    return NULL;
}

static m2_ctx* ctx_for_room(u64 room_id)
{
    for (int i = 1; i <= SCE_NP_MATCHING2_CTX_MAX; i++)
        if (s_ctx[i].in_use && s_ctx[i].room.in && s_ctx[i].room.id == room_id) return &s_ctx[i];
    return NULL;
}

static u16 ctx_id(const m2_ctx* c) { return (u16)(c - s_ctx); }

/* ---------------------------------------------------------------------------
 * Building SDK structures
 * -----------------------------------------------------------------------*/

/* SceNpUserInfo2 { SceNpId npId (36); u32 onlineName; u32 avatarUrl; }      44 bytes */
static void rb_userinfo(rb_t* r, u32 at, const char* online_id)
{
    memcpy(r->b + at, online_id, strnlen(online_id, 16));      /* npId.handle.data */
    u32 name = rb_alloc(r, 48, 4);                              /* SceNpOnlineName */
    memcpy(r->b + name, online_id, strnlen(online_id, 16));
    u32 url = rb_alloc(r, 128, 4);                              /* SceNpAvatarUrl, empty */
    rb_ptr(r, at + 36, name);
    rb_ptr(r, at + 40, url);
}

/* SceNpMatching2RoomMemberDataInternal                                     88 bytes
 *   0 next | 4 userInfo (44) | 48 joinDate u64 | 56 memberId u16 | 60 flagAttr
 *   64 teamId | 68 roomGroup | 72 natType | 76 binAttrInternal | 80 num
 * Checked against Simpsons Arcade, which reads the NP ID at +4 and the member
 * id at +56 to assign its transport channels. With joinDate ahead of
 * userInfo (an earlier guess) it read the top of a pointer as the member id,
 * matched no peer, and never sent a packet. */
static u32 rb_member(rb_t* r, const m2_room* room, const m2_member* m)
{
    u32 o = rb_alloc(r, 88, 8);
    rb_userinfo(r, o + 4, m->online_id);
    rb_64(r, o + 48, m->joined);
    rb_16(r, o + 56, m->id);
    rb_32(r, o + 60, m->id == room->owner ? MEMBER_FLAG_OWNER : 0);
    rb_8(r, o + 72, 2);   /* NAT type 2 */
    u32 n, arr = rb_attrs(r, &m->data, 1, &n);
    if (n) rb_ptr(r, o + 76, arr);
    rb_32(r, o + 80, n);
    return o;
}

/* SceNpMatching2RoomDataInternal                                           72 bytes
 *   0 serverId u16 | 4 worldId | 8 lobbyId u64 | 16 roomId u64
 *   24 passwordSlotMask u64 | 32 maxSlot | 36 memberList {members, num, me, owner}
 *   52 roomGroup | 56 roomGroupNum | 60 flagAttr | 64 binAttrInternal | 68 num */
static u32 rb_room_internal(rb_t* r, const m2_room* room)
{
    u32 o = rb_alloc(r, 72, 8);
    rb_16(r, o, SERVER_ID);
    rb_32(r, o + 4, WORLD_ID);
    rb_64(r, o + 16, room->id);
    rb_32(r, o + 32, room->max);
    u32 prev = 0, first = 0;
    for (int i = 0; i < room->n; i++) {
        u32 mo = rb_member(r, room, &room->m[i]);
        if (prev) rb_ptr(r, prev, mo); else first = mo;
        prev = mo;
        if (room->m[i].id == room->me) rb_ptr(r, o + 44, mo);
        if (room->m[i].id == room->owner) rb_ptr(r, o + 48, mo);
    }
    if (first) rb_ptr(r, o + 36, first);
    rb_32(r, o + 40, (u32)room->n);
    rb_32(r, o + 60, room->flags);
    u32 n, arr = rb_attrs(r, &room->in_attrs, 1, &n);
    if (n) rb_ptr(r, o + 64, arr);
    rb_32(r, o + 68, n);
    return o;
}

/* ---------------------------------------------------------------------------
 * Signaling and room events
 * -----------------------------------------------------------------------*/

static void signal_member(m2_ctx* c, u16 member, u16 event)
{
    printf("[sceNpMatching2] -> signaling 0x%04X member %u\n", event, member);
    /* (ctxId, roomId, memberId, event, errorCode, arg) */
    queue_cb(c->sig_cb, ctx_id(c), c->room.id, member, event, 0, c->sig_arg, 0, 0);
}

static void room_event(m2_ctx* c, u16 event, rb_t* img)
{
    printf("[sceNpMatching2] -> room event 0x%04X\n", event);
    u32 size = img ? img->len : 0, key = img ? store_event(img) : 0;
    /* (ctxId, roomId, event, eventKey, errorCode, dataSize, arg) */
    queue_cb(c->room_cb, ctx_id(c), c->room.id, event, key, 0, size, c->room_arg, 0);
}

/* "Established" is deferred. On PSN it arrives only after the peers have
 * reached each other -- hundreds of milliseconds after the member joined --
 * and titles count on having processed the join by then. Simpsons Arcade's
 * host gives a newcomer a transport channel while handling MemberJoined; told
 * "established" in the same breath, it had no channel for the peer yet, never
 * registered its address, and never answered it ("HOMER IS NOT RESPONDING").
 * PS3_NP_SIGNALING_DELAY_MS sets the delay; default 1000. */
#define SIG_PENDING_MAX 16
static struct { int used; u16 ctx; u64 room; u16 member; u64 due; } s_sig[SIG_PENDING_MAX];

static u64 now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000u + (u64)ts.tv_nsec / 1000000u;
#endif
}

static void establish_later(m2_ctx* c, u16 member)
{
    static long delay = -1;
    if (delay < 0) {
        const char* e = getenv("PS3_NP_SIGNALING_DELAY_MS");
        delay = e ? atol(e) : 1000;
    }
    LOCK();
    for (int i = 0; i < SIG_PENDING_MAX; i++)
        if (!s_sig[i].used) {
            s_sig[i].used = 1;
            s_sig[i].ctx = ctx_id(c);
            s_sig[i].room = c->room.id;
            s_sig[i].member = member;
            s_sig[i].due = now_ms() + (u64)delay;
            break;
        }
    UNLOCK();
}

static void cancel_signal(u16 ctx, u16 member)   /* member 0 = all */
{
    LOCK();
    for (int i = 0; i < SIG_PENDING_MAX; i++)
        if (s_sig[i].used && s_sig[i].ctx == ctx && (!member || s_sig[i].member == member))
            s_sig[i].used = 0;
    UNLOCK();
}

static void m2_tick(void)
{
    u64 now = now_ms();
    for (int i = 0; i < SIG_PENDING_MAX; i++) {
        LOCK();
        int due = s_sig[i].used && s_sig[i].due <= now;
        u16 ctx = s_sig[i].ctx, member = s_sig[i].member;
        u64 room = s_sig[i].room;
        if (due) s_sig[i].used = 0;
        UNLOCK();
        if (!due) continue;
        m2_ctx* c = ctx_get(ctx);
        if (c && c->room.in && c->room.id == room && room_member(&c->room, member))
            signal_member(c, member, SIG_EV_Established);
    }
}

/* SceNpMatching2RoomMemberUpdateInfo { u32 member; u8 eventCause; u8 pad[3];
 * PresenceOptionData optData (20) }                                       28 bytes */
static void member_event(m2_ctx* c, const m2_member* m, u16 event, u8 cause)
{
    rb_t img = {0};
    u32 o = rb_alloc(&img, 28, 8);
    rb_ptr(&img, o, rb_member(&img, &c->room, m));
    rb_8(&img, o + 4, cause);
    room_event(c, event, &img);
}

/* Leaving, by choice or not: every connection goes down. */
static void room_gone(m2_ctx* c)
{
    cancel_signal(ctx_id(c), 0);
    for (int i = 0; i < c->room.n; i++)
        if (c->room.m[i].id != c->room.me) signal_member(c, c->room.m[i].id, SIG_EV_Dead);
    c->room.in = 0;
}

/* SceNpMatching2RoomDataInternalUpdateInfo                                36 bytes
 *   0 newRoomDataInternal | 4 newFlagAttr* | 8 prevFlagAttr* | 12/16 password masks
 *   20 newRoomGroup | 24 num | 28 newRoomBinAttrInternal (ptr to ptrs) | 32 num
 * Every member gets this, the one who made the change included: Simpsons
 * Arcade's host writes its slot map with SetRoomDataInternal as a player
 * joins, and assigns the newcomer a transport channel when its own update
 * event comes back. psnr pushes to the others; the writer's is raised here. */
static void internal_updated(m2_ctx* c, int flags_changed, u32 prev_flags, int bins_changed)
{
    m2_room* r = &c->room;
    rb_t img = {0};
    u32 o = rb_alloc(&img, 36, 8);
    rb_ptr(&img, o, rb_room_internal(&img, r));
    if (flags_changed) {
        u32 f = rb_alloc(&img, 8, 4);
        rb_32(&img, f, r->flags);
        rb_32(&img, f + 4, prev_flags);
        rb_ptr(&img, o + 4, f);
        rb_ptr(&img, o + 8, f + 4);
    }
    if (bins_changed) {
        u32 n, arr = rb_attrs(&img, &r->in_attrs, 1, &n);
        if (n) {
            u32 pp = rb_alloc(&img, n * 4, 4);
            for (u32 i = 0; i < n; i++) rb_ptr(&img, pp + i * 4, arr + i * 12);
            rb_ptr(&img, o + 28, pp);
        }
        rb_32(&img, o + 32, n);
    }
    room_event(c, ROOM_EV_UpdatedRoomDataInternal, &img);
}

static void on_push(const psnr_msg* m)
{
    if (m->len < 8) return;
    u64 room_id = psnr_get64(m->data);
    LOCK();
    m2_ctx* c = ctx_for_room(room_id);
    UNLOCK();
    if (!c) return;
    m2_room* r = &c->room;
    const u8 *p = m->data + 8, *end = m->data + m->len;

    switch (m->type) {
    case PSNR_MEMBER_JOINED: {
        int owner;
        if (r->n >= MEMBER_MAX || !parse_member(&r->m[r->n], p, end, &owner)) return;
        m2_member* nm = &r->m[r->n++];
        printf("[sceNpMatching2] room %llu: %s joined as member %u\n",
               (unsigned long long)r->id, nm->online_id, nm->id);
        member_event(c, nm, ROOM_EV_MemberJoined, 0);
        establish_later(c, nm->id);
        break;
    }
    case PSNR_MEMBER_LEFT: {
        if (end - p < 4) return;
        u16 gone = psnr_get16(p), owner = psnr_get16(p + 2);
        m2_member* gm = room_member(r, gone);
        if (!gm) return;
        printf("[sceNpMatching2] room %llu: %s left\n", (unsigned long long)r->id, gm->online_id);
        cancel_signal(ctx_id(c), gone);
        signal_member(c, gone, SIG_EV_Dead);
        member_event(c, gm, ROOM_EV_MemberLeft, CAUSE_LEAVE_ACTION);
        *gm = r->m[--r->n];
        if (owner != r->owner) {
            /* SceNpMatching2RoomOwnerUpdateInfo { u16 prev; u16 new; u8 cause; pad; optData } */
            rb_t img = {0};
            u32 o = rb_alloc(&img, 28, 4);
            rb_16(&img, o, r->owner);
            rb_16(&img, o + 2, owner);
            rb_8(&img, o + 4, CAUSE_LEAVE_ACTION);
            r->owner = owner;
            room_event(c, ROOM_EV_RoomOwnerChanged, &img);
        }
        break;
    }
    case PSNR_ROOM_DATA: {
        if (end - p < 3) return;
        u8 which = p[0];
        u16 l = psnr_get16(p + 1);
        if (p + 3 + l > end) return;
        u32 prev_flags = r->flags;
        if (which == 0) { attrs_unpack(&r->ext, p + 3, l); return; }   /* external: no event */
        if (which == 1) attrs_unpack(&r->in_attrs, p + 3, l);
        if (which == 2 && l == 4) r->flags = psnr_get32(p + 3);
        internal_updated(c, which == 2, prev_flags, which == 1);
        break;
    }
    case PSNR_ROOM_MSG: {
        if (end - p < 6) return;
        u16 from = psnr_get16(p), to = psnr_get16(p + 2), l = psnr_get16(p + 4);
        if (p + 6 + l > end) return;
        m2_member* src = room_member(r, from);
        /* SceNpMatching2RoomMessageInfo { u8 filtered; u8 castType; u8 pad[2];
         * u32 dst; u32 srcMember (UserInfo2); u32 msg; u32 msgLen }        20 bytes */
        rb_t img = {0};
        u32 o = rb_alloc(&img, 20, 4);
        rb_8(&img, o + 1, to ? CAST_UNICAST : CAST_BROADCAST);
        u32 dst = rb_alloc(&img, 8, 4);
        rb_16(&img, dst, to ? to : r->me);
        rb_ptr(&img, o + 4, dst);
        u32 ui = rb_alloc(&img, 44, 4);
        rb_userinfo(&img, ui, src ? src->online_id : "");
        rb_ptr(&img, o + 8, ui);
        u32 msg = rb_alloc(&img, l ? l : 1, 4);
        memcpy(img.b + msg, p + 6, l);
        rb_ptr(&img, o + 12, msg);
        rb_32(&img, o + 16, l);
        u32 size = img.len, key = store_event(&img);
        /* (ctxId, roomId, srcMemberId, event, eventKey, errorCode, dataSize, arg) */
        queue_cb(c->msg_cb, ctx_id(c), r->id, from, ROOM_MSG_EV_Message, key, 0, size, c->msg_arg);
        break;
    }
    case PSNR_KICKED: {
        /* SceNpMatching2RoomUpdateInfo { u8 cause; pad[3]; s32 errorCode; optData }  28 bytes */
        rb_t img = {0};
        u32 o = rb_alloc(&img, 28, 4);
        rb_8(&img, o, CAUSE_KICKOUT_ACTION);
        room_gone(c);
        room_event(c, ROOM_EV_Kickedout, &img);
        break;
    }
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Lifecycle and contexts
 * -----------------------------------------------------------------------*/

static s32 m2_init(void)
{
    if (s_init) return (s32)SCE_NP_MATCHING2_ERROR_ALREADY_INITIALIZED;
    memset(s_ctx, 0, sizeof(s_ctx));
    s_init = 1;
    np_psnr_on_push(on_push);
    np_psnr_on_tick(m2_tick);
    printf("[sceNpMatching2] Init (%s)\n", np_psnr_enabled() ? "online via psnr" : "offline");
    return CELL_OK;
}

s32 sceNp2Init(u32 poolSize, u32 pool)
{
    /* sceNp2Init is the NP init for titles built against libsceNp2: without
     * it the manager answers NOT_INITIALIZED to every call. */
    s32 r = sceNpInit(poolSize, (void*)(uintptr_t)pool);
    return r == (s32)SCE_NP_ERROR_ALREADY_INITIALIZED ? CELL_OK : r;
}

s32 sceNp2Term(void) { return sceNpTerm(); }

s32 sceNpMatching2Init(u32 poolSize, s32 threadPriority, s32 threadStackSize)
{
    (void)poolSize; (void)threadPriority; (void)threadStackSize;
    return m2_init();
}

s32 sceNpMatching2Init2(u32 stackSize, s32 priority, u32 param)
{
    (void)stackSize; (void)priority; (void)param;
    return m2_init();
}

s32 sceNpMatching2Term(void)  { s_init = 0; return CELL_OK; }
s32 sceNpMatching2Term2(void) { s_init = 0; return CELL_OK; }

/* SceNpCommunicationId { char data[9]; char term; u8 num; u8 pad } -> "NPWR01444_00" */
s32 sceNpMatching2CreateContext(u32 npId, u32 commId, u32 passPhrase, u32 ctxIdOut, s32 option)
{
    (void)npId; (void)passPhrase; (void)option;
    if (!s_init) return (s32)SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    if (!ctxIdOut || !commId) return (s32)SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    for (u16 i = 1; i <= SCE_NP_MATCHING2_CTX_MAX; i++) {
        if (s_ctx[i].in_use) continue;
        memset(&s_ctx[i], 0, sizeof(m2_ctx));
        s_ctx[i].in_use = 1;
        char data[10] = {0};
        memcpy(data, vm_ptr8(commId), 9);
        snprintf(s_ctx[i].comm_id, sizeof(s_ctx[i].comm_id), "%s_%02u", data, vm_read8(commId + 10));
        vm_write16(ctxIdOut, i);
        printf("[sceNpMatching2] CreateContext(%s) -> %u\n", s_ctx[i].comm_id, i);
        return CELL_OK;
    }
    return (s32)SCE_NP_MATCHING2_ERROR_OUT_OF_MEMORY;
}

s32 sceNpMatching2DestroyContext(u16 ctxId)
{
    m2_ctx* c = ctx_get(ctxId);
    if (!c) return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    c->in_use = 0;
    return CELL_OK;
}

/* (ctxId, event, eventCause, errorCode, arg) */
static s32 start(u16 ctxId)
{
    m2_ctx* c = ctx_get(ctxId);
    if (!c) return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    if (np_psnr_connect(c->comm_id) == 0) {
        c->started = 1;
        queue_cb(c->ctx_cb, ctxId, CTX_EV_Start, CAUSE_CONTEXT_ACTION, 0, c->ctx_arg, 0, 0, 0);
    } else {
        queue_cb(c->ctx_cb, ctxId, CTX_EV_StartOver, CAUSE_CONTEXT_ERROR,
                 (u64)(u32)SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE, c->ctx_arg, 0, 0, 0);
    }
    return CELL_OK;
}

s32 sceNpMatching2ContextStart(u16 ctxId)                     { return start(ctxId); }
s32 sceNpMatching2ContextStartAsync(u16 ctxId, u32 timeout)   { (void)timeout; return start(ctxId); }

s32 sceNpMatching2ContextStop(u16 ctxId)
{
    m2_ctx* c = ctx_get(ctxId);
    if (!c) return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    c->started = 0;
    queue_cb(c->ctx_cb, ctxId, CTX_EV_Stop, CAUSE_CONTEXT_ACTION, 0, c->ctx_arg, 0, 0, 0);
    return CELL_OK;
}

s32 sceNpMatching2ContextStopAsync(u16 ctxId, u32 timeout) { (void)timeout; return sceNpMatching2ContextStop(ctxId); }

/* SceNpMatching2RequestOptParam { u32 cbFunc; u32 cbFuncArg; u32 timeout; u16 appReqId; pad } */
s32 sceNpMatching2SetDefaultRequestOptParam(u16 ctxId, u32 opt)
{
    m2_ctx* c = ctx_get(ctxId);
    if (!c) return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    if (!opt) return (s32)SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    c->def_cb = vm_read32(opt);
    c->def_arg = vm_read32(opt + 4);
    return CELL_OK;
}

/* Written out rather than generated by a macro: gen_hle_nids.py finds
 * exports by their definitions, and a macro hides them from it. */
s32 sceNpMatching2RegisterContextCallback(u16 ctxId, u32 cb, u32 arg)
{
    m2_ctx* c = ctx_get(ctxId);
    if (!c) return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    c->ctx_cb = cb; c->ctx_arg = arg;
    return CELL_OK;
}

s32 sceNpMatching2RegisterRoomEventCallback(u16 ctxId, u32 cb, u32 arg)
{
    m2_ctx* c = ctx_get(ctxId);
    if (!c) return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    c->room_cb = cb; c->room_arg = arg;
    return CELL_OK;
}

s32 sceNpMatching2RegisterRoomMessageCallback(u16 ctxId, u32 cb, u32 arg)
{
    m2_ctx* c = ctx_get(ctxId);
    if (!c) return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    c->msg_cb = cb; c->msg_arg = arg;
    return CELL_OK;
}

s32 sceNpMatching2RegisterSignalingCallback(u16 ctxId, u32 cb, u32 arg)
{
    m2_ctx* c = ctx_get(ctxId);
    if (!c) return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    c->sig_cb = cb; c->sig_arg = arg;
    return CELL_OK;
}

/* Lobbies aren't served: registering is accepted, nothing ever fires. */
s32 sceNpMatching2RegisterLobbyEventCallback(u16 ctxId, u32 cb, u32 arg)
{ (void)cb; (void)arg; return ctx_get(ctxId) ? CELL_OK : (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND; }
s32 sceNpMatching2RegisterLobbyMessageCallback(u16 ctxId, u32 cb, u32 arg)
{ (void)cb; (void)arg; return ctx_get(ctxId) ? CELL_OK : (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND; }

/* ---------------------------------------------------------------------------
 * Server and world: one of each, answered locally
 * -----------------------------------------------------------------------*/

#define REQUIRE_STARTED(c, ctxId)                                           \
    m2_ctx* c = ctx_get(ctxId);                                             \
    if (!c) return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;           \
    if (!c->started || !np_psnr_connected())                                \
        return (s32)SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE;

s32 sceNpMatching2GetServerIdListLocal(u16 ctxId, u32 serverId, u32 maxNum)
{
    REQUIRE_STARTED(c, ctxId);
    if (serverId && maxNum) vm_write16(serverId, SERVER_ID);
    return 1;   /* the number of servers */
}

/* request { u16 serverId } -> SceNpMatching2GetServerInfoResponse { u16 serverId; u8 status; u8 pad } */
s32 sceNpMatching2GetServerInfo(u16 ctxId, u32 req, u32 opt, u32 reqId)
{
    REQUIRE_STARTED(c, ctxId);
    (void)req;
    rb_t img = {0};
    u32 o = rb_alloc(&img, 4, 4);
    rb_16(&img, o, SERVER_ID);
    rb_8(&img, o + 2, SERVER_STATUS_AVAILABLE);
    return answer_now(op_new(c, ctxId, EV_GetServerInfo, opt, reqId), 0, &img);
}

s32 sceNpMatching2CreateServerContext(u16 ctxId, u32 req, u32 opt, u32 reqId)
{
    REQUIRE_STARTED(c, ctxId);
    (void)req;
    return answer_now(op_new(c, ctxId, EV_CreateServerContext, opt, reqId), 0, NULL);
}

s32 sceNpMatching2DeleteServerContext(u16 ctxId, u32 req, u32 opt, u32 reqId)
{
    REQUIRE_STARTED(c, ctxId);
    (void)req;
    return answer_now(op_new(c, ctxId, EV_DeleteServerContext, opt, reqId), 0, NULL);
}

/* -> { u32 world*; u32 worldNum } + SceNpMatching2World (60 bytes:
 *   worldId, numOfLobby, maxNumOfTotalLobbyMember, curNumOfTotalLobbyMember,
 *   curNumOfRoom, curNumOfTotalRoomMember, withEntitlementId u8 + pad, entitlementId[32]) */
s32 sceNpMatching2GetWorldInfoList(u16 ctxId, u32 req, u32 opt, u32 reqId)
{
    REQUIRE_STARTED(c, ctxId);
    (void)req;
    rb_t img = {0};
    u32 o = rb_alloc(&img, 8, 4);
    u32 w = rb_alloc(&img, 60, 4);
    rb_32(&img, w, WORLD_ID);
    rb_ptr(&img, o, w);
    rb_32(&img, o + 4, 1);
    return answer_now(op_new(c, ctxId, EV_GetWorldInfoList, opt, reqId), 0, &img);
}

/* ---------------------------------------------------------------------------
 * Rooms
 * -----------------------------------------------------------------------*/

static s32 psnr_error_to_m2(const psnr_msg* m)
{
    if (!m->type) return (s32)SCE_NP_MATCHING2_ERROR_NOT_CONNECTED;
    if (m->type != PSNR_ERROR || m->len < 4) return (s32)SCE_NP_MATCHING2_SERVER_ERROR_BAD_REQUEST;
    switch (psnr_get32(m->data)) {
        case PSNR_E_NOT_FOUND:  return (s32)SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_ROOM;
        case PSNR_E_ROOM_FULL:  return (s32)SCE_NP_MATCHING2_SERVER_ERROR_ROOM_FULL;
        case PSNR_E_NOT_OWNER:  return (s32)SCE_NP_MATCHING2_SERVER_ERROR_FORBIDDEN;
        default:                return (s32)SCE_NP_MATCHING2_SERVER_ERROR_BAD_REQUEST;
    }
}

static int int_match(u8 op, u32 have, u32 want)
{
    switch (op) {
        case 1: return have == want;   /* EQ */
        case 2: return have != want;   /* NE */
        case 3: return have <  want;   /* LT */
        case 4: return have <= want;   /* LE */
        case 5: return have >  want;   /* GT */
        case 6: return have >= want;   /* GE */
        default: return 1;
    }
}

/* ROOM_LIST: total u16 | count u16 | (room u64 | owner [16] | cur u8 | max u8 | flags u32 | ext blob)... */
static void on_search(void* user, const psnr_msg* m)
{
    m2_op* op = (m2_op*)user;
    if (m->type != PSNR_ROOM_LIST || m->len < 4) {
        answer(op, psnr_error_to_m2(m), NULL);
        free(op);
        return;
    }
    const u8 *p = m->data + 4, *end = m->data + m->len;
    u16 count = psnr_get16(m->data + 2);

    /* SceNpMatching2SearchRoomResponse { Range { startIndex, total, size }; u32 roomDataExternal } */
    rb_t img = {0};
    u32 o = rb_alloc(&img, 16, 4);
    u32 matched = 0, listed = 0, prev = 0;
    for (u16 i = 0; i < count && p + 34 <= end; i++) {
        u64 room_id = psnr_get64(p);
        char owner[17] = {0};
        memcpy(owner, p + 8, 16);
        u8 cur = p[24], max = p[25];
        u32 flags = psnr_get32(p + 26) | (cur >= max ? ROOM_FLAG_FULL : 0);
        u16 l = psnr_get16(p + 30);
        const u8* ext_p = p + 32;
        p += 32 + l;
        if (p > end) break;

        m2_attrs ext;
        attrs_unpack(&ext, ext_p, l);
        if ((flags & op->flag_filter) != (op->flag_attr & op->flag_filter)) continue;
        int ok = 1;
        for (int f = 0; f < op->nifl && ok; f++) {
            const m2_attr* a = attr_find(&ext, op->ifl[f].id);
            ok = a && a->kind == 0 && int_match(op->ifl[f].op, a->num, op->ifl[f].num);
        }
        if (!ok) continue;
        if (++matched < op->start || listed >= op->max) continue;

        /* SceNpMatching2RoomDataExternal                                   88 bytes
         *   0 next | 4 serverId u16 | 8 worldId | 12 publicSlotNum u16 | 14 privateSlotNum
         *   16 lobbyId u64 | 24 roomId u64 | 32 openPublicSlotNum | 34 maxSlot
         *   36 openPrivateSlotNum | 38 curMemberNum | 40 passwordSlotMask u64
         *   48 owner (UserInfo2*) | 52 roomGroup | 56 roomGroupNum | 60 flagAttr
         *   64 searchableIntAttrExternal | 68 num | 72 searchableBinAttrExternal
         *   76 num | 80 binAttrExternal | 84 num */
        u32 rd = rb_alloc(&img, 88, 8);
        rb_16(&img, rd + 4, SERVER_ID);
        rb_32(&img, rd + 8, WORLD_ID);
        rb_16(&img, rd + 12, max);
        rb_64(&img, rd + 24, room_id);
        rb_16(&img, rd + 32, (u16)(max > cur ? max - cur : 0));
        rb_16(&img, rd + 34, max);
        rb_16(&img, rd + 38, cur);
        u32 ui = rb_alloc(&img, 44, 4);
        rb_userinfo(&img, ui, owner);
        rb_ptr(&img, rd + 48, ui);
        rb_32(&img, rd + 60, flags);

        /* The attributes the search asked for, split the SDK's three ways:
         * searchable ints (0x4C-0x53), searchable bin (0x54), bin external. */
        m2_attrs want_int = {0}, want_sbin = {0}, want_bin = {0};
        for (int k = 0; k < op->nattr; k++) {
            const m2_attr* a = attr_find(&ext, op->attr_ids[k]);
            if (!a) continue;
            m2_attrs* dst = a->kind == 0 ? &want_int : (a->id == 0x54 ? &want_sbin : &want_bin);
            if (dst->n < ATTR_MAX) dst->a[dst->n++] = *a;
        }
        u32 n, arr;
        if ((arr = rb_attrs(&img, &want_int, 0, &n)) != 0) rb_ptr(&img, rd + 64, arr);
        rb_32(&img, rd + 68, n);
        if ((arr = rb_attrs(&img, &want_sbin, 1, &n)) != 0) rb_ptr(&img, rd + 72, arr);
        rb_32(&img, rd + 76, n);
        if ((arr = rb_attrs(&img, &want_bin, 1, &n)) != 0) rb_ptr(&img, rd + 80, arr);
        rb_32(&img, rd + 84, n);

        if (prev) rb_ptr(&img, prev, rd); else rb_ptr(&img, o + 12, rd);
        prev = rd;
        listed++;
    }
    rb_32(&img, o, op->start);
    rb_32(&img, o + 4, matched);
    rb_32(&img, o + 8, listed);
    printf("[sceNpMatching2] SearchRoom: %u of %u rooms match, %u listed\n", matched, count, listed);
    answer(op, 0, &img);
    free(op);
}

/* SceNpMatching2SearchRoomRequest                                         56 bytes
 *   0 option | 4 worldId | 8 lobbyId u64 | 16 range { startIndex, max }
 *   24 flagFilter | 28 flagAttr | 32 intFilter* | 36 num | 40 binFilter* | 44 num
 *   48 attrId* (u16[]) | 52 num
 * IntSearchFilter { u8 op; u8 pad[3]; IntAttr attr }                      12 bytes */
s32 sceNpMatching2SearchRoom(u16 ctxId, u32 req, u32 opt, u32 reqId)
{
    REQUIRE_STARTED(c, ctxId);
    if (!req) return (s32)SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    m2_op* op = op_new(c, ctxId, EV_SearchRoom, opt, reqId);
    op->start = vm_read32(req + 16);
    op->max = vm_read32(req + 20);
    if (op->start == 0) op->start = 1;
    op->flag_filter = vm_read32(req + 24);
    op->flag_attr = vm_read32(req + 28);
    u32 ifl = vm_read32(req + 32), nifl = vm_read32(req + 36);
    for (u32 i = 0; i < nifl && i < 8 && ifl; i++) {
        op->ifl[i].op = vm_read8(ifl + i * 12);
        op->ifl[i].id = vm_read16(ifl + i * 12 + 4);
        op->ifl[i].num = vm_read32(ifl + i * 12 + 8);
        op->nifl++;
    }
    /* ponytail: bin filters are ignored (every room passes them); Simpsons
     * sends none. Match on attr bytes when a title does. */
    u32 ids = vm_read32(req + 48), nids = vm_read32(req + 52);
    for (u32 i = 0; i < nids && i < 16 && ids; i++) op->attr_ids[op->nattr++] = vm_read16(ids + i * 2);

    u8 body[4];
    psnr_put16(body, 0);
    psnr_put16(body + 2, 100);   /* ponytail: first 100 rooms, filtered here */
    if (!np_psnr_request(PSNR_SEARCH_ROOMS, body, 4, on_search, op)) {
        free(op);
        return (s32)SCE_NP_MATCHING2_ERROR_NOT_CONNECTED;
    }
    return CELL_OK;
}

/* ROOM_JOINED -> CreateJoinRoom / JoinRoom response { u32 roomDataInternal } */
static void on_joined(void* user, const psnr_msg* m)
{
    m2_op* op = (m2_op*)user;
    m2_ctx* c = ctx_get(op->ctx);
    if (!c || m->type != PSNR_ROOM_JOINED || parse_joined(&c->room, m) != 0) {
        answer(op, c ? psnr_error_to_m2(m) : (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND, NULL);
        free(op);
        return;
    }
    m2_room* r = &c->room;
    printf("[sceNpMatching2] %s room %llu as member %u (%d/%u)\n",
           op->event == EV_CreateJoinRoom ? "created" : "joined",
           (unsigned long long)r->id, r->me, r->n, r->max);
    rb_t img = {0};
    u32 o = rb_alloc(&img, 4, 8);
    rb_ptr(&img, o, rb_room_internal(&img, r));
    answer(op, 0, &img);
    /* Mesh signaling: everyone already here is reachable now. */
    for (int i = 0; i < r->n; i++)
        if (r->m[i].id != r->me) establish_later(c, r->m[i].id);
    free(op);
}

/* SceNpMatching2CreateJoinRoomRequest                                    112 bytes
 *   0 worldId | 8 lobbyId u64 | 16 maxSlot | 20 flagAttr
 *   24 roomBinAttrInternal* | 28 num | 32 roomSearchableIntAttrExternal* | 36 num
 *   40 roomSearchableBinAttrExternal* | 44 num | 48 roomBinAttrExternal* | 52 num
 *   56 roomPassword* | 60 groupConfig* | 64 num | 68 passwordSlotMask*
 *   72 allowedUser* | 76 num | 80 blockedUser* | 84 num | 88 joinRoomGroupLabel*
 *   92 roomMemberBinAttrInternal* | 96 num | 100 teamId | 104 sigOptParam* */
s32 sceNpMatching2CreateJoinRoom(u16 ctxId, u32 req, u32 opt, u32 reqId)
{
    REQUIRE_STARTED(c, ctxId);
    if (!req) return (s32)SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    u8* body = (u8*)malloc(3 * ATTR_MAX * (BIN_MAX + 8) + 32);
    m2_attrs ext = {0}, in = {0}, mine = {0};
    attrs_read_int(&ext, vm_read32(req + 32), vm_read32(req + 36));
    attrs_read_bin(&ext, vm_read32(req + 40), vm_read32(req + 44));
    attrs_read_bin(&ext, vm_read32(req + 48), vm_read32(req + 52));
    attrs_read_bin(&in, vm_read32(req + 24), vm_read32(req + 28));
    attrs_read_bin(&mine, vm_read32(req + 92), vm_read32(req + 96));

    /* max u8 | flags u32 | external blob | internal blob | member data blob */
    u8* p = body;
    u32 max = vm_read32(req + 16);
    *p++ = (u8)(max > 255 ? 255 : max);
    p = psnr_put32(p, vm_read32(req + 20));
    u32 l = attrs_pack(&ext, p + 2); psnr_put16(p, (u16)l); p += 2 + l;
    l = attrs_pack(&in, p + 2);      psnr_put16(p, (u16)l); p += 2 + l;
    l = attrs_pack(&mine, p + 2);    psnr_put16(p, (u16)l); p += 2 + l;
    m2_op* op = op_new(c, ctxId, EV_CreateJoinRoom, opt, reqId);
    u32 sent = np_psnr_request(PSNR_CREATE_ROOM, body, (u32)(p - body), on_joined, op);
    free(body);
    if (!sent) { free(op); return (s32)SCE_NP_MATCHING2_ERROR_NOT_CONNECTED; }
    return CELL_OK;
}

/* SceNpMatching2JoinRoomRequest { u64 roomId; u32 roomPassword*; u32 joinRoomGroupLabel*;
 *   u32 roomMemberBinAttrInternal*; u32 num; PresenceOptionData optData; u8 teamId; ... } */
s32 sceNpMatching2JoinRoom(u16 ctxId, u32 req, u32 opt, u32 reqId)
{
    REQUIRE_STARTED(c, ctxId);
    if (!req) return (s32)SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    u8* body = (u8*)malloc(ATTR_MAX * (BIN_MAX + 8) + 16);
    m2_attrs mine = {0};
    attrs_read_bin(&mine, vm_read32(req + 16), vm_read32(req + 20));
    u8* p = psnr_put64(body, vm_read64(req));
    u32 l = attrs_pack(&mine, p + 2); psnr_put16(p, (u16)l); p += 2 + l;
    m2_op* op = op_new(c, ctxId, EV_JoinRoom, opt, reqId);
    u32 sent = np_psnr_request(PSNR_JOIN_ROOM, body, (u32)(p - body), on_joined, op);
    free(body);
    if (!sent) { free(op); return (s32)SCE_NP_MATCHING2_ERROR_NOT_CONNECTED; }
    return CELL_OK;
}

/* Replies that carry nothing back: OK or ERROR. */
static void on_done(void* user, const psnr_msg* m)
{
    m2_op* op = (m2_op*)user;
    m2_ctx* c = ctx_get(op->ctx);
    s32 err = m->type == PSNR_OK ? 0 : psnr_error_to_m2(m);
    rb_t img = {0};
    if (op->event == EV_LeaveRoom && c) {
        /* SceNpMatching2LeaveRoomResponse { u64 roomId } */
        u32 o = rb_alloc(&img, 8, 8);
        rb_64(&img, o, c->room.id);
        room_gone(c);
    }
    answer(op, err, img.len ? &img : NULL);
    if (op->event == EV_SetRoomDataInternal && !err && c && c->room.in)
        internal_updated(c, op->flags_changed, op->prev_flags, op->bins_changed);
    rb_free(&img);
    free(op);
}

static s32 send_simple(m2_ctx* c, u16 ctxId, u16 event, u32 opt, u32 reqId,
                       uint8_t type, const u8* body, u32 len)
{
    m2_op* op = op_new(c, ctxId, event, opt, reqId);
    if (!np_psnr_request(type, body, len, on_done, op)) {
        free(op);
        return (s32)SCE_NP_MATCHING2_ERROR_NOT_CONNECTED;
    }
    return CELL_OK;
}

/* SceNpMatching2LeaveRoomRequest { u64 roomId; PresenceOptionData optData; pad } */
s32 sceNpMatching2LeaveRoom(u16 ctxId, u32 req, u32 opt, u32 reqId)
{
    REQUIRE_STARTED(c, ctxId);
    if (!req) return (s32)SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    u8 body[8];
    psnr_put64(body, vm_read64(req));
    return send_simple(c, ctxId, EV_LeaveRoom, opt, reqId, PSNR_LEAVE_ROOM, body, 8);
}

/* SceNpMatching2KickoutRoomMemberRequest { u64 roomId; u16 target; u8 blockKickFlag; ... } */
s32 sceNpMatching2KickoutRoomMember(u16 ctxId, u32 req, u32 opt, u32 reqId)
{
    REQUIRE_STARTED(c, ctxId);
    if (!req) return (s32)SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    u8 body[10];
    psnr_put16(psnr_put64(body, vm_read64(req)), vm_read16(req + 8));
    return send_simple(c, ctxId, EV_KickoutRoomMember, opt, reqId, PSNR_KICK_MEMBER, body, 10);
}

/* SceNpMatching2SetRoomDataExternalRequest { u64 roomId; searchableInt*, num;
 *   searchableBin*, num; binExternal*, num }. Attrs merge into what's there. */
s32 sceNpMatching2SetRoomDataExternal(u16 ctxId, u32 req, u32 opt, u32 reqId)
{
    REQUIRE_STARTED(c, ctxId);
    if (!req || !c->room.in) return (s32)SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    u8* body = (u8*)malloc(ATTR_MAX * (BIN_MAX + 8) + 16);
    attrs_read_int(&c->room.ext, vm_read32(req + 8), vm_read32(req + 12));
    attrs_read_bin(&c->room.ext, vm_read32(req + 16), vm_read32(req + 20));
    attrs_read_bin(&c->room.ext, vm_read32(req + 24), vm_read32(req + 28));
    u8* p = psnr_put64(body, c->room.id);
    *p++ = 0;
    u32 l = attrs_pack(&c->room.ext, p + 2); psnr_put16(p, (u16)l); p += 2 + l;
    s32 r = send_simple(c, ctxId, EV_SetRoomDataExternal, opt, reqId, PSNR_SET_ROOM_DATA, body, (u32)(p - body));
    free(body);
    return r;
}

/* SceNpMatching2SetRoomDataInternalRequest { u64 roomId; u32 flagFilter; u32 flagAttr;
 *   binAttrInternal*, num; ... }. Flags and attrs are separate psnr writes;
 *   the title hears back once, on the last. */
s32 sceNpMatching2SetRoomDataInternal(u16 ctxId, u32 req, u32 opt, u32 reqId)
{
    REQUIRE_STARTED(c, ctxId);
    if (!req || !c->room.in) return (s32)SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    u8* body = (u8*)malloc(ATTR_MAX * (BIN_MAX + 8) + 16);
    u32 filter = vm_read32(req + 8), attr = vm_read32(req + 12);
    u32 bins = vm_read32(req + 16), nbins = vm_read32(req + 20);
    u32 prev_flags = c->room.flags;
    if (filter) {
        c->room.flags = (c->room.flags & ~filter) | (attr & filter);
        u8* p = psnr_put64(body, c->room.id);
        *p++ = 2;
        p = psnr_put16(p, 4);
        p = psnr_put32(p, c->room.flags);
        np_psnr_request(PSNR_SET_ROOM_DATA, body, (u32)(p - body), NULL, NULL);
    }
    attrs_read_bin(&c->room.in_attrs, bins, nbins);
    u8* p = psnr_put64(body, c->room.id);
    *p++ = 1;
    u32 l = attrs_pack(&c->room.in_attrs, p + 2); psnr_put16(p, (u16)l); p += 2 + l;
    m2_op* op = op_new(c, ctxId, EV_SetRoomDataInternal, opt, reqId);
    op->prev_flags = prev_flags;
    op->flags_changed = filter && c->room.flags != prev_flags;
    op->bins_changed = nbins != 0;
    s32 r = CELL_OK;
    if (!np_psnr_request(PSNR_SET_ROOM_DATA, body, (u32)(p - body), on_done, op)) {
        free(op);
        r = (s32)SCE_NP_MATCHING2_ERROR_NOT_CONNECTED;
    }
    free(body);
    return r;
}

/* SceNpMatching2SendRoomMessageRequest { u64 roomId; u8 castType; u8 pad[3];
 *   RoomMessageDestination dst (8: unicast u16 | multicast { u16* ids; u32 num });
 *   u32 msg*; u32 msgLen; s32 option } */
s32 sceNpMatching2SendRoomMessage(u16 ctxId, u32 req, u32 opt, u32 reqId)
{
    REQUIRE_STARTED(c, ctxId);
    if (!req) return (s32)SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    u64 room = vm_read64(req);
    u8 cast = vm_read8(req + 8);
    u32 msg = vm_read32(req + 20), len = vm_read32(req + 24);
    if (len > 0xFF00) return (s32)SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    u16 targets[MEMBER_MAX];
    int nt = 0;
    if (cast == CAST_UNICAST) targets[nt++] = vm_read16(req + 12);
    else if (cast == CAST_MULTICAST) {
        u32 ids = vm_read32(req + 12), n = vm_read32(req + 16);
        for (u32 i = 0; i < n && nt < MEMBER_MAX && ids; i++) targets[nt++] = vm_read16(ids + i * 2);
    } else targets[nt++] = 0;   /* broadcast / team: everyone */

    u8* body = (u8*)malloc(len + 12);
    s32 r = CELL_OK;
    for (int i = 0; i < nt; i++) {
        u8* p = psnr_put64(body, room);
        p = psnr_put16(p, targets[i]);
        p = psnr_put16(p, (u16)len);
        if (len) memcpy(p, vm_ptr8(msg), len);
        if (i == nt - 1)
            r = send_simple(c, ctxId, EV_SendRoomMessage, opt, reqId, PSNR_ROOM_MESSAGE, body, 12 + len);
        else
            np_psnr_request(PSNR_ROOM_MESSAGE, body, 12 + len, NULL, NULL);
    }
    free(body);
    return r;
}

s32 sceNpMatching2GetEventData(u16 ctxId, u32 eventKey, u32 buf, u32 bufLen)
{
    (void)ctxId;
    if (!buf) return (s32)SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    LOCK();
    int slot = (int)(eventKey % EVENT_RING);
    if (s_events[slot].key != eventKey || !s_events[slot].img.len) {
        UNLOCK();
        return (s32)SCE_NP_MATCHING2_ERROR_EVENT_DATA_NOT_FOUND;
    }
    rb_t* img = &s_events[slot].img;
    u32 n = img->len < bufLen ? img->len : bufLen;
    memcpy(vm_ptr8(buf), img->b, n);
    for (u32 i = 0; i < img->nrel; i++) {
        u32 at = img->rel[i];
        if (at + 4 <= n) vm_write32(buf + at, vm_read32(buf + at) + buf);
    }
    UNLOCK();
    return (s32)n;
}

s32 sceNpMatching2AbortRequest(u16 ctxId, u32 reqId)
{
    /* ponytail: the answer still arrives; titles abort on the way out. */
    (void)reqId;
    return ctx_get(ctxId) ? CELL_OK : (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
}

/* ---------------------------------------------------------------------------
 * Signaling
 * -----------------------------------------------------------------------*/

s32 sceNpMatching2SignalingGetConnectionStatus(u16 ctxId, u64 roomId, u16 memberId,
                                               u32 connStatus, u32 peerAddr, u32 peerPort)
{
    m2_ctx* c = ctx_get(ctxId);
    if (!c) return (s32)SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND;
    m2_member* m = (c->room.in && c->room.id == roomId) ? room_member(&c->room, memberId) : NULL;
    if (connStatus) vm_write32(connStatus, m ? SIG_CONN_ACTIVE : SIG_CONN_INACTIVE);
    if (peerAddr) memcpy(vm_ptr8(peerAddr), m ? m->ip : (const u8*)"\0\0\0\0", 4);
    if (peerPort) vm_write16(peerPort, m ? m->port : 0);
    if (m) printf("[sceNpMatching2] signaling: member %u (%s) at %u.%u.%u.%u:%u\n", memberId,
                  m->online_id, m->ip[0], m->ip[1], m->ip[2], m->ip[3], m->port);
    return CELL_OK;
}

/* request { u64 roomId; u16 memberId } -> SceNpMatching2SignalingGetPingInfoResponse
 * { u16 serverId; u8 pad[2]; u32 worldId; u64 roomId; u32 rtt (usec) } */
s32 sceNpMatching2SignalingGetPingInfo(u16 ctxId, u32 req, u32 opt, u32 reqId)
{
    REQUIRE_STARTED(c, ctxId);
    rb_t img = {0};
    u32 o = rb_alloc(&img, 24, 8);
    rb_16(&img, o, SERVER_ID);
    rb_32(&img, o + 4, WORLD_ID);
    rb_64(&img, o + 8, req ? vm_read64(req) : c->room.id);
    rb_32(&img, o + 16, 30000);   /* ponytail: a nominal 30 ms; measure over the P2P socket if a title shows it */
    return answer_now(op_new(c, ctxId, EV_SignalingGetPingInfo, opt, reqId), 0, &img);
}

/* ---------------------------------------------------------------------------
 * Lobbies and the old stub's names: not served
 * -----------------------------------------------------------------------*/

s32 sceNpMatching2SearchLobby(u16 ctxId, u32 opt, u32 reqId)
{ (void)ctxId; (void)opt; (void)reqId; return (s32)SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE; }
s32 sceNpMatching2JoinLobby(u16 ctxId, u64 lobbyId, u32 opt, u32 reqId)
{ (void)ctxId; (void)lobbyId; (void)opt; (void)reqId; return (s32)SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE; }
s32 sceNpMatching2LeaveLobby(u16 ctxId, u32 reqId)
{ (void)ctxId; (void)reqId; return CELL_OK; }
s32 sceNpMatching2CreateRoom(u16 ctxId, u32 opt, u32 reqId)
{ (void)ctxId; (void)opt; (void)reqId; return (s32)SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE; }

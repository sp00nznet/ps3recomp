/*
 * ps3recomp - sceNpScore (leaderboards) over psnr
 *
 * Title contexts and transactions are local ids. The requests go to the psnr
 * server's leaderboards (np_psnr.h); offline, every transaction finishes with
 * SCE_NP_COMMUNITY_SERVER_ERROR_... so titles show "leaderboards unavailable".
 *
 * Async is the SDK's poll model: *Async returns at once, the title polls
 * PollAsync(transId) until it returns 0 and reads the result there. PollAsync
 * pumps psnr itself, so a title that polls from a worker thread without
 * calling cellSysutilCheckCallback still finishes. The synchronous forms send
 * and then pump until the reply lands.
 *
 * The ranking calls take up to 15 arguments; everything past r10 comes from
 * the caller's parameter save area (PPC64 ELFv1: arg n>8 at SP+112+8*(n-9)),
 * so those are ctx handlers registered by np_score_register_ctx() -- called
 * from ppu_sysprx_register().
 *
 * Layouts (SDK ABI):
 *   SceNpScoreRankData                                             128 bytes
 *     0 npId (36) | 36 onlineName (48) | 84 pcId | 88 serialRank | 92 rank
 *     96 highestRank | 104 scoreValue s64 | 112 hasGameData | 120 recordDate u64
 *   SceNpScorePlayerRankData { s32 hasData; pad[4]; RankData }    136 bytes
 *   SceNpScoreComment { char utf8Comment[64] }                     64 bytes
 */

#include "sceNp.h"
#include "np_psnr.h"
#include "../../runtime/ppu/ppu_context.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../include/ps3emu/nid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static void sleep_10ms(void) { Sleep(10); }
#else
#  include <time.h>
static void sleep_10ms(void) { struct timespec ts = { 0, 10000000L }; nanosleep(&ts, NULL); }
#endif

#define SCORE_ERROR_NO_SERVER      0x8002A1A1u   /* community server error: unavailable */
#define SCORE_ERROR_INVALID_ID     0x8002A106u
#define SCORE_ERROR_ABORTED        0x8002A10Au

#define TITLE_MAX 8
#define TRANS_MAX 32

enum { T_FREE, T_IDLE, T_PENDING, T_DONE };

typedef struct {
    int state;
    int title;
    s32 result;
    /* where the reply goes */
    u32 tmp_rank_ea;
    u32 rank_ea, comment_ea, total_ea, last_sort_ea;
    u32 array_num;
    int by_id;
} score_trans;

static int         s_titles[TITLE_MAX + 1];   /* 1-based ids */
static score_trans s_trans[TRANS_MAX + 1];

extern const char* np_psnr_online_id(void);

/* ---------------------------------------------------------------------------
 * Contexts
 * -----------------------------------------------------------------------*/

s32 sceNpScoreCreateTitleCtx(u32 commId, u32 passphrase, u32 selfNpId)
{
    (void)passphrase; (void)selfNpId;
    for (int i = 1; i <= TITLE_MAX; i++)
        if (!s_titles[i]) {
            s_titles[i] = 1;
            /* psnr scopes boards by the title's communication ID, which the
             * connection already carries; connect now in case Matching2 hasn't. */
            if (commId) {
                char id[16] = {0}, data[10] = {0};
                memcpy(data, vm_ptr8(commId), 9);
                snprintf(id, sizeof(id), "%s_%02u", data, vm_read8(commId + 10));
                np_psnr_connect(id);
            }
            return i;
        }
    return (s32)SCE_NP_ERROR_OUT_OF_MEMORY;
}

s32 sceNpScoreDestroyTitleCtx(s32 titleCtxId)
{
    if (titleCtxId < 1 || titleCtxId > TITLE_MAX || !s_titles[titleCtxId])
        return (s32)SCORE_ERROR_INVALID_ID;
    s_titles[titleCtxId] = 0;
    return CELL_OK;
}

s32 sceNpScoreCreateTransactionCtx(s32 titleCtxId)
{
    if (titleCtxId < 1 || titleCtxId > TITLE_MAX || !s_titles[titleCtxId])
        return (s32)SCORE_ERROR_INVALID_ID;
    for (int i = 1; i <= TRANS_MAX; i++)
        if (s_trans[i].state == T_FREE) {
            memset(&s_trans[i], 0, sizeof(s_trans[i]));
            s_trans[i].state = T_IDLE;
            s_trans[i].title = titleCtxId;
            return i;
        }
    return (s32)SCE_NP_ERROR_OUT_OF_MEMORY;
}

static score_trans* trans_get(s32 id)
{
    return (id >= 1 && id <= TRANS_MAX && s_trans[id].state != T_FREE) ? &s_trans[id] : NULL;
}

s32 sceNpScoreDestroyTransactionCtx(s32 transId)
{
    score_trans* t = trans_get(transId);
    if (!t) return (s32)SCORE_ERROR_INVALID_ID;
    t->state = T_FREE;   /* a reply still in flight finds the slot free and is dropped */
    return CELL_OK;
}

s32 sceNpScoreSetTimeout(s32 id, u32 timeout)  { (void)id; (void)timeout; return CELL_OK; }
s32 sceNpScoreSetPlayerCharacterId(s32 id, s32 pcId) { (void)id; (void)pcId; return CELL_OK; }

s32 sceNpScoreAbortTransaction(s32 transId)
{
    score_trans* t = trans_get(transId);
    if (!t) return (s32)SCORE_ERROR_INVALID_ID;
    if (t->state == T_PENDING) { t->state = T_DONE; t->result = (s32)SCORE_ERROR_ABORTED; }
    return CELL_OK;
}

/* 0 = finished (result in *result), 1 = still running. */
s32 sceNpScorePollAsync(s32 transId, u32 result)
{
    score_trans* t = trans_get(transId);
    if (!t) return (s32)SCORE_ERROR_INVALID_ID;
    if (t->state == T_PENDING) np_psnr_pump();
    if (t->state == T_PENDING) return 1;
    if (result) vm_write32(result, (u32)t->result);
    return 0;
}

s32 sceNpScoreWaitAsync(s32 transId, u32 result)
{
    score_trans* t = trans_get(transId);
    if (!t) return (s32)SCORE_ERROR_INVALID_ID;
    for (int i = 0; i < 1000 && t->state == T_PENDING; i++) {   /* ~10 s */
        np_psnr_pump();
        if (t->state == T_PENDING) sleep_10ms();
    }
    if (t->state == T_PENDING) { t->state = T_DONE; t->result = (s32)SCORE_ERROR_NO_SERVER; }
    if (result) vm_write32(result, (u32)t->result);
    return t->result;
}

/* ---------------------------------------------------------------------------
 * Requests
 * -----------------------------------------------------------------------*/

/* Finish a transaction that never reached the server. */
static s32 fail_now(score_trans* t, s32 err)
{
    t->state = T_DONE;
    t->result = err;
    return CELL_OK;
}

static s32 trans_id_of(score_trans* t) { return (s32)(t - s_trans); }

static void on_recorded(void* user, const psnr_msg* m)
{
    score_trans* t = trans_get((s32)(uintptr_t)user);
    if (!t || t->state != T_PENDING) return;
    t->state = T_DONE;
    if (m->type != PSNR_SCORE_RECORDED || m->len < 4) { t->result = (s32)SCORE_ERROR_NO_SERVER; return; }
    if (t->tmp_rank_ea) vm_write32(t->tmp_rank_ea, psnr_get32(m->data));
    t->result = CELL_OK;
}

/* RECORD_SCORE: board u32 | order u8 | score s64 | comment blob */
static s32 record(s32 transId, u32 boardId, s64 score, u32 comment, u32 tmpRank)
{
    score_trans* t = trans_get(transId);
    u8 body[4 + 1 + 8 + 2 + 64];
    if (!t) return (s32)SCORE_ERROR_INVALID_ID;
    t->state = T_PENDING;
    t->tmp_rank_ea = tmpRank;
    if (!np_psnr_connected()) return fail_now(t, (s32)SCORE_ERROR_NO_SERVER);

    u8* p = psnr_put32(body, boardId);
    /* ponytail: every board ranks higher-is-better. A title with lap-time
     * boards needs its order passed (1) -- a per-title board table. */
    *p++ = 0;
    p = psnr_put64(p, (u64)score);
    size_t cl = comment ? strnlen((const char*)vm_ptr8(comment), 63) : 0;
    p = psnr_put16(p, (u16)cl);
    if (cl) { memcpy(p, vm_ptr8(comment), cl); p += cl; }
    printf("[sceNpScore] RecordScore(board %u, %lld)\n", boardId, (long long)score);
    if (!np_psnr_request(PSNR_RECORD_SCORE, body, (u32)(p - body), on_recorded,
                         (void*)(uintptr_t)trans_id_of(t)))
        return fail_now(t, (s32)SCORE_ERROR_NO_SERVER);
    return CELL_OK;
}

/* (transId, boardId, score, comment*, gameInfo*, tmpRank*, prio, option) */
s32 sceNpScoreRecordScoreAsync(s32 transId, u32 boardId, s64 score, u32 comment,
                               u32 gameInfo, u32 tmpRank, s32 prio, u32 option)
{
    (void)gameInfo; (void)prio; (void)option;
    return record(transId, boardId, score, comment, tmpRank);
}

/* (transId, boardId, score, comment*, gameInfo*, tmpRank*, option) */
s32 sceNpScoreRecordScore(s32 transId, u32 boardId, s64 score, u32 comment,
                          u32 gameInfo, u32 tmpRank, u32 option)
{
    (void)gameInfo; (void)option;
    s32 r = record(transId, boardId, score, comment, tmpRank);
    return r < 0 ? r : sceNpScoreWaitAsync(transId, 0);
}

/* Write one SceNpScoreRankData from a RANKING entry. */
static const u8* put_rank(u32 at, const u8* e, const u8* end, u32 comment_at)
{
    if (e + 4 + 16 + 8 + 2 > end) return NULL;
    u32 rank = psnr_get32(e);
    char id[17] = {0};
    memcpy(id, e + 4, 16);
    u64 score = psnr_get64(e + 20);
    u16 cl = psnr_get16(e + 28);
    const u8* c = e + 30;
    if (c + cl + 8 > end) return NULL;
    u64 when = psnr_get64(c + cl);

    memset(vm_ptr8(at), 0, 128);
    memcpy(vm_ptr8(at), id, strnlen(id, 16));           /* npId.handle */
    memcpy(vm_ptr8(at + 36), id, strnlen(id, 16));      /* onlineName */
    vm_write32(at + 88, rank);                          /* serialRank */
    vm_write32(at + 92, rank);                          /* rank */
    vm_write32(at + 96, rank);                          /* highestRank */
    vm_write64(at + 104, score);
    /* CellRtcTick: microseconds since 0001-01-01 */
    vm_write64(at + 120, when ? (when + 62135596800ull) * 1000000ull : 0);
    if (comment_at) {
        memset(vm_ptr8(comment_at), 0, 64);
        memcpy(vm_ptr8(comment_at), c, cl < 63 ? cl : 63);
    }
    return c + cl + 8;
}

static void on_ranking(void* user, const psnr_msg* m)
{
    score_trans* t = trans_get((s32)(uintptr_t)user);
    if (!t || t->state != T_PENDING) return;
    t->state = T_DONE;
    if (m->type != PSNR_RANKING || m->len < 6) { t->result = (s32)SCORE_ERROR_NO_SERVER; return; }
    u32 total = psnr_get32(m->data);
    u16 n = psnr_get16(m->data + 4);
    const u8 *e = m->data + 6, *end = m->data + m->len;
    u32 filled = 0;
    for (u16 i = 0; i < n && i < t->array_num && e; i++) {
        u32 stride = t->by_id ? 136 : 128;
        u32 at = t->rank_ea + i * stride;
        if (t->by_id) vm_write32(at, psnr_get32(e) ? 1 : 0);   /* hasData */
        e = put_rank(t->by_id ? at + 8 : at, e, end, t->comment_ea ? t->comment_ea + i * 64 : 0);
        filled++;
    }
    if (t->total_ea) vm_write32(t->total_ea, total);
    if (t->last_sort_ea) vm_write64(t->last_sort_ea, 0);
    /* The SDK returns the number of entries written. */
    t->result = (s32)filled;
}

static u64 stack_arg(ppu_context* ctx, int n)   /* n >= 9 */
{
    return vm_read64((u32)ctx->gpr[1] + 112u + 8u * (u32)(n - 9));
}

/* GetRankingByRange[Async](transId, boardId, startSerialRank, rankArray*, rankArraySize,
 *   commentArray*, commentArraySize, infoArray*, infoArraySize, arrayNum,
 *   lastSortDate*, totalRecord*, [prio,] option) */
static void ranking_by_range(ppu_context* ctx)
{
    score_trans* t = trans_get((s32)ctx->gpr[3]);
    u8 body[10];
    if (!t) { ctx->gpr[3] = (u64)(s64)(s32)SCORE_ERROR_INVALID_ID; return; }
    t->state = T_PENDING;
    t->by_id = 0;
    t->rank_ea = (u32)ctx->gpr[6];
    t->comment_ea = (u32)ctx->gpr[8];
    t->array_num = (u32)stack_arg(ctx, 10);
    t->last_sort_ea = (u32)stack_arg(ctx, 11);
    t->total_ea = (u32)stack_arg(ctx, 12);
    ctx->gpr[3] = 0;
    if (!np_psnr_connected()) { fail_now(t, (s32)SCORE_ERROR_NO_SERVER); return; }
    u8* p = psnr_put32(body, (u32)ctx->gpr[4]);
    p = psnr_put32(p, (u32)ctx->gpr[5] ? (u32)ctx->gpr[5] : 1);
    p = psnr_put16(p, (u16)(t->array_num > 100 ? 100 : t->array_num));
    if (!np_psnr_request(PSNR_GET_RANKING, body, 10, on_ranking, (void*)(uintptr_t)trans_id_of(t)))
        fail_now(t, (s32)SCORE_ERROR_NO_SERVER);
}

static void hle_ranking_by_range_async(ppu_context* ctx) { ranking_by_range(ctx); }
static void hle_ranking_by_range(ppu_context* ctx)
{
    s32 id = (s32)ctx->gpr[3];
    ranking_by_range(ctx);
    if ((s32)ctx->gpr[3] == 0) ctx->gpr[3] = (u64)(s64)sceNpScoreWaitAsync(id, 0);
}

/* GetRankingByNpId[Async](transId, boardId, npIdArray*, npIdArraySize, rankArray*,
 *   rankArraySize, commentArray*, commentArraySize, infoArray*, infoArraySize,
 *   arrayNum, lastSortDate*, totalRecord*, [prio,] option) */
static void ranking_by_npid(ppu_context* ctx)
{
    score_trans* t = trans_get((s32)ctx->gpr[3]);
    if (!t) { ctx->gpr[3] = (u64)(s64)(s32)SCORE_ERROR_INVALID_ID; return; }
    t->state = T_PENDING;
    t->by_id = 1;
    t->rank_ea = (u32)ctx->gpr[7];
    t->comment_ea = (u32)ctx->gpr[9];
    t->array_num = (u32)stack_arg(ctx, 11);
    t->last_sort_ea = (u32)stack_arg(ctx, 12);
    t->total_ea = (u32)stack_arg(ctx, 13);
    ctx->gpr[3] = 0;
    if (!np_psnr_connected()) { fail_now(t, (s32)SCORE_ERROR_NO_SERVER); return; }
    u32 ids = (u32)ctx->gpr[5], n = t->array_num > 100 ? 100 : t->array_num;
    u8* body = (u8*)malloc(6 + 16 * n);
    u8* p = psnr_put32(body, (u32)ctx->gpr[4]);
    p = psnr_put16(p, (u16)n);
    for (u32 i = 0; i < n; i++, p += 16) {   /* SceNpId is 36 bytes; the handle leads */
        memset(p, 0, 16);
        memcpy(p, vm_ptr8(ids + i * 36), strnlen((const char*)vm_ptr8(ids + i * 36), 16));
    }
    if (!np_psnr_request(PSNR_GET_RANKING_BY_ID, body, (u32)(p - body), on_ranking,
                         (void*)(uintptr_t)trans_id_of(t)))
        fail_now(t, (s32)SCORE_ERROR_NO_SERVER);
    free(body);
}

static void hle_ranking_by_npid_async(ppu_context* ctx) { ranking_by_npid(ctx); }
static void hle_ranking_by_npid(ppu_context* ctx)
{
    s32 id = (s32)ctx->gpr[3];
    ranking_by_npid(ctx);
    if ((s32)ctx->gpr[3] == 0) ctx->gpr[3] = (u64)(s64)sceNpScoreWaitAsync(id, 0);
}

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

void np_score_register_ctx(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreGetRankingByRangeAsync"),
                         "sceNpScoreGetRankingByRangeAsync", hle_ranking_by_range_async);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreGetRankingByRange"),
                         "sceNpScoreGetRankingByRange", hle_ranking_by_range);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreGetRankingByNpIdAsync"),
                         "sceNpScoreGetRankingByNpIdAsync", hle_ranking_by_npid_async);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreGetRankingByNpId"),
                         "sceNpScoreGetRankingByNpId", hle_ranking_by_npid);
}

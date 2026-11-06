/*
 * test_avc2_load - cellSysutilAvc2Load and the setters.
 *
 *   cc -std=gnu17 -I include -o test_avc2_load libs/network/tests/test_avc2_load.c
 *
 * Compiles cellSysutilAvc2.c into this file with sysutil stubbed: the stub
 * records what would be queued for the title's callback. Checks that the
 * synchronous Load is defined under the name whose NID titles import
 * (0xC6C93D62; the HLE table is built from these names), that it stores the
 * callback so LOAD_SUCCEEDED and a later JoinChatRequest's JOIN_SUCCEEDED
 * reach it with the title's userdata, and that Unload and the setters return OK.
 */
#include "../cellSysutilAvc2.c"
#include "ps3emu/nid.h"

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

#define QMAX 8
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

/* The queued call k: callback opd, (event_id, event_param, userdata). */
static void expect(int k, u32 opd, u32 event, u32 userdata)
{
    assert(k < s_nq);
    assert(s_q[k].opd == opd && s_q[k].a[0] == event && s_q[k].a[1] == 0 && s_q[k].a[2] == userdata);
}

int main(void)
{
    static uint8_t mem[0x1000];
    vm_base = mem;
    const u32 CB = 0x8000, UD = 0x1234;

    /* The names resolve to the NIDs titles import. */
    assert(ps3_compute_nid("cellSysutilAvc2Load") == 0xC6C93D62u);
    assert(ps3_compute_nid("cellSysutilAvc2Unload") == 0x38F33624u);

    assert(cellSysutilAvc2InitParam(0x120, 0x100) == CELL_OK);
    assert(cellSysutilAvc2Load(1, 0, CB, UD, 0x100) == CELL_OK);
    assert(s_nq == 1);
    expect(0, CB, AVC2_EVENT_LOAD_SUCCEEDED, UD);

    /* The stored callback gets the join. */
    assert(cellSysutilAvc2JoinChatRequest(0x200) == CELL_OK);
    assert(s_nq == 2);
    expect(1, CB, AVC2_EVENT_JOIN_SUCCEEDED, UD);

    assert(cellSysutilAvc2SetAttribute(0x300) == CELL_OK);
    assert(cellSysutilAvc2SetSpeakerVolumeLevel(0x304) == CELL_OK);
    assert(cellSysutilAvc2SetSpeakerMuting(1) == CELL_OK);
    assert(cellSysutilAvc2SetVoiceMuting(1) == CELL_OK);
    assert(cellSysutilAvc2StartVoiceDetection() == CELL_OK);
    assert(cellSysutilAvc2StopVoiceDetection() == CELL_OK);
    assert(s_nq == 2);

    assert(cellSysutilAvc2LeaveChatRequest() == CELL_OK);
    assert(cellSysutilAvc2Unload() == CELL_OK);
    assert(s_nq == 4);
    expect(2, CB, AVC2_EVENT_LEAVE_SUCCEEDED, UD);
    expect(3, CB, AVC2_EVENT_UNLOAD_SUCCEEDED, UD);

    printf("test_avc2_load: ok (%d events)\n", s_nq);
    return 0;
}

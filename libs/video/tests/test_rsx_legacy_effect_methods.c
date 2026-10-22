/*
 * ps3recomp - rsx_process_method's effects outside rsx_state
 *
 * With the draw engine up and no vtable backend, the FIFO walker hands
 * rsx_process_method only the methods in cellGcmSys.c's
 * gcm_legacy_method_has_effect: 0x0050, 0x0104, 0x0180, 0x01A4, 0x1D6C,
 * 0x1D70, 0x1D74. That is safe only while every other method touches nothing
 * but the rsx_state it is handed. With no backend registered, this walks every
 * method 0x0000..0x1FFC (subchannel bits are not passed) with several data
 * words and checks that none outside the set
 *   - writes guest memory,
 *   - moves g_rsx_last_reference, or
 *   - changes the hidden notify / semaphore context, which a notify and two
 *     label writes issued after it would show.
 * It also checks that each method in the set does have an effect, so the set
 * stays no larger than it needs to be.
 *
 * Build (standalone):
 *   cc -std=gnu17 -I include -I libs/video libs/video/tests/test_rsx_legacy_effect_methods.c \
 *      libs/video/rsx_commands.c -o /tmp/test_rsx_legacy_effect_methods
 */
#include "rsx_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t ppu_hle_inject_base;
void ps3_ms(const char* key) { (void)key; }
uint32_t gcm_io_offset_to_ea(uint32_t io) { return 0x10000000u + io; }
unsigned long long ps3_ms_now(void) { return 1234; }
extern u32 g_rsx_last_reference;

/* Every guest write, in order. */
static u32 wr_addr[64], wr_val[64], wr_n;
void vm_write32(uint32_t addr, uint32_t value)
{
    if (wr_n < 64) { wr_addr[wr_n] = addr; wr_val[wr_n] = value; }
    wr_n++;
}

/* The set, or the one given on the command line (tools/test_rsx_legacy_effect_methods.py
 * passes the cases of gcm_legacy_method_has_effect). */
static u32 effect_set[16] = { 0x0050u, 0x0104u, 0x0180u, 0x01A4u, 0x1D6Cu, 0x1D70u, 0x1D74u };
static u32 n_effect = 7;
static int is_effect(u32 m)
{
    for (u32 i = 0; i < n_effect; i++) if (effect_set[i] == m) return 1;
    return 0;
}

static rsx_state st;

/* A notify context that writes, a label window context and a label offset. */
static void prime(u32 sem_ctx)
{
    rsx_process_method(&st, 0x0180u, 0x6660420Fu);
    rsx_process_method(&st, 0x01A4u, sem_ctx);
    rsx_process_method(&st, 0x1D6Cu, 0x00000120u);
}
/* What the hidden context now does: a notify and both label writes. */
static void probe(void)
{
    rsx_process_method(&st, 0x0104u, 0u);
    rsx_process_method(&st, 0x1D70u, 0x11223344u);
    rsx_process_method(&st, 0x1D74u, 0x55667788u);
}
static u32 base_addr[64], base_val[64], base_n;
static void snapshot_baseline(u32 sem_ctx)
{
    rsx_state_init(&st);
    prime(sem_ctx);
    wr_n = 0;
    probe();
    base_n = wr_n;
    memcpy(base_addr, wr_addr, sizeof base_addr);
    memcpy(base_val, wr_val, sizeof base_val);
}
static int probe_matches_baseline(void)
{
    if (wr_n != base_n) return 0;
    for (u32 i = 0; i < wr_n && i < 64; i++)
        if (wr_addr[i] != base_addr[i] || wr_val[i] != base_val[i]) return 0;
    return 1;
}

int main(int argc, char** argv)
{
    if (argc > 1) {
        n_effect = 0;
        for (int i = 1; i < argc && n_effect < 16; i++) effect_set[n_effect++] = (u32)strtoul(argv[i], NULL, 16);
    }
    static const u32 datas[] = { 0u, 1u, 0xFFFFFFFFu, 0x6660420Fu, 0xFEED0000u, 0xFEED0001u,
                                 0x12345678u, 0x00000120u };
    static const u32 sem_ctxs[] = { 0xFEED0000u, 0xFEED0001u };
    int fails = 0;
    unsigned checked = 0;

    for (u32 c = 0; c < 2; c++) {
        snapshot_baseline(sem_ctxs[c]);
        if (base_n == 0) { printf("[FAIL] probe writes nothing (ctx 0x%08X)\n", sem_ctxs[c]); fails++; }
        for (u32 m = 0; m <= 0x1FFCu; m += 4u) {
            if (is_effect(m)) continue;
            for (u32 d = 0; d < sizeof datas / sizeof datas[0]; d++) {
                rsx_state_init(&st);
                prime(sem_ctxs[c]);
                g_rsx_last_reference = 0xA5A5A5A5u;
                wr_n = 0;
                rsx_process_method(&st, m, datas[d]);
                const int wrote = wr_n != 0;
                const int ref_moved = g_rsx_last_reference != 0xA5A5A5A5u;
                wr_n = 0;
                probe();
                const int ctx_changed = !probe_matches_baseline();
                checked++;
                if (wrote || ref_moved || ctx_changed) {
                    if (fails < 20)
                        printf("[FAIL] method 0x%04X data 0x%08X ctx 0x%08X: %s%s%s\n", m, datas[d],
                               sem_ctxs[c], wrote ? "writes guest memory " : "",
                               ref_moved ? "moves the reference " : "",
                               ctx_changed ? "changes the notify/semaphore context" : "");
                    fails++;
                }
            }
        }
    }
    printf("[%s] %u method/data/context cases outside the set act only on rsx_state\n",
           fails ? "FAIL" : "PASS", checked);

    /* Each method in the set has an effect the filter must keep. */
    {
        rsx_state_init(&st);
        g_rsx_last_reference = 0;
        rsx_process_method(&st, 0x0050u, 7u);
        const int ref = g_rsx_last_reference == 7u;
        snapshot_baseline(0xFEED0000u);
        const u32 label_writes = base_n;  /* 0x0104 notify + 0x1D70 + 0x1D74 */
        /* Another notify context, semaphore context or label offset moves the probe. */
        rsx_state_init(&st); prime(0xFEED0000u); rsx_process_method(&st, 0x0180u, 0u);
        wr_n = 0; probe(); const int notify_ctx = !probe_matches_baseline();
        rsx_state_init(&st); prime(0xFEED0000u); rsx_process_method(&st, 0x01A4u, 0xFEED0001u);
        wr_n = 0; probe(); const int sem_ctx = !probe_matches_baseline();
        rsx_state_init(&st); prime(0xFEED0000u); rsx_process_method(&st, 0x1D6Cu, 0x40u);
        wr_n = 0; probe(); const int sem_off = !probe_matches_baseline();
        const int ok = ref && label_writes == 6u && notify_ctx && sem_ctx && sem_off;
        printf("[%s] every method in the set has an effect outside rsx_state "
               "(ref %d, probe writes %u, notify ctx %d, semaphore ctx %d, label offset %d)\n",
               ok ? "PASS" : "FAIL", ref, label_writes, notify_ctx, sem_ctx, sem_off);
        fails += !ok;
    }

    printf("%s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}

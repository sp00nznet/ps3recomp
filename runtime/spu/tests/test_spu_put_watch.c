/* PS3RECOMP_SPU_PUT_WATCH through the production transfer path in spu_dma.h:
 * single PUTs, GETs, list-element PUTs and SDCRZ, with the watched word inside
 * or outside the range. The driver (tools/test_spu_put_watch.py) runs this
 * with the variable set and unset and counts the reports on stderr. Only
 * external observation/coherence dependencies are inert. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../spu_dma.h"

static uint8_t memory[0x40000];
uint8_t* vm_base = memory;
uint8_t g_vm_page_bitmap[65536 / 8];
int g_cri_video_dma;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_barrier_sync_watch;
void (*g_spu_put_hook)(uint32_t, uint32_t);
void* volatile g_sn_defer_ctx;
volatile unsigned g_sn_defer;
unsigned long long ps3_ms_now(void) { return 0; }
unsigned spu_sn_defer_ticks(void) { return 0; }
unsigned* spu_ls_watch_list(int* n) { *n = 0; return NULL; }
void spu_overlay_note_get(spu_context* c, uint32_t ea, const uint8_t* p, uint32_t n)
{ (void)c; (void)ea; (void)p; (void)n; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_coh_notify_write_except(uint32_t ea, const void* c) { (void)ea; (void)c; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockguard_check(spu_context* c, uint32_t ea, const uint8_t* p, uint32_t n, const char* what)
{ (void)c; (void)ea; (void)p; (void)n; (void)what; }
void spu_ch_wake(spu_context* c) { (void)c; }

static void check(int ok, const char* what)
{
    if (!ok) { fprintf(stdout, "FAIL: %s\n", what); exit(2); }
}

#define WATCH 0x20044u   /* the driver sets PS3RECOMP_SPU_PUT_WATCH to this */

int main(void)
{
    memset(g_vm_page_bitmap, 0xFF, sizeof g_vm_page_bitmap);
    spu_context* ctx = (spu_context*)calloc(1, sizeof(*ctx));
    check(ctx != NULL, "allocate context");
    spu_context_init(ctx, 3);
    ctx->image_id = 7;
    ctx->pc = 0x2340;
    ctx->resident_code[0].image_id = 11;
    ctx->resident_code[0].lsa = 0x2000;
    ctx->resident_code[0].size = 0x1000;
    for (unsigned i = 0; i < 0x200; i++) ctx->ls[0x1000 + i] = (uint8_t)(0x40 + i);

    /* 1: a PUT covering the word -> one report, and the bytes land. */
    check(mfc_do_transfer(ctx, 0x1000, 0x20040, 16, MFC_PUT_CMD) == 0, "PUT covering");
    check(memcmp(memory + 0x20040, ctx->ls + 0x1000, 16) == 0, "PUT bytes");
    /* 2: a PUT ending just before the word -> nothing. */
    check(mfc_do_transfer(ctx, 0x1000, 0x20000, 0x44, MFC_PUT_CMD) == 0, "PUT before");
    /* 3: a PUT starting just after the word -> nothing. */
    check(mfc_do_transfer(ctx, 0x1000, 0x20048, 16, MFC_PUT_CMD) == 0, "PUT after");
    /* 4: a GET covering the word -> nothing (it does not write guest memory). */
    check(mfc_do_transfer(ctx, 0x1100, 0x20040, 16, MFC_GET_CMD) == 0, "GET covering");
    /* 5: a two-element list PUT whose second element covers the word -> one report. */
    spu_ls_write32(ctx, 0x100, 16); spu_ls_write32(ctx, 0x104, 0x30000);
    spu_ls_write32(ctx, 0x108, 16); spu_ls_write32(ctx, 0x10C, 0x20040);
    check(mfc_run_list(ctx, 0x100, 2, 0x1000, 0, MFC_PUTL_CMD, 5) == 0, "list PUT");
    check(memcmp(memory + 0x20040, ctx->ls + 0x1010, 16) == 0, "list element bytes");
    /* 6: SDCRZ over the word's 128-byte block -> one report, word zeroed. */
    check(mfc_do_transfer(ctx, 0, 0x20000, 128, MFC_SDCRZ_CMD) == 0, "SDCRZ covering");
    check(memory[WATCH] == 0 && memory[WATCH + 3] == 0, "SDCRZ zeroed");

    free(ctx);
    puts("OK: put-watch transfers ran");
    return 0;
}

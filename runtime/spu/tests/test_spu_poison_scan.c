/* The [POISON-DMA] report of the production PUT path (spu_dma.h). PUTs into
 * the singleton region are always reported with the payload scan's result;
 * PUTs elsewhere are scanned for the poison word, and reported, only when
 * SPU_POISON_DMA_WATCH is set. The caller sets the environment and checks
 * stderr. Only external observation/coherence dependencies are inert. */
#ifndef _WIN32
#include <sys/mman.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../spu_dma.h"

uint8_t* vm_base;
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
{ if (!ok) { fprintf(stdout, "FAIL: %s\n", what); exit(2); } }

/* PUT 64 bytes from LS 0x1000 to `ea`, with the poison word at offset 32 or
 * not, and check the bytes landed. */
static void put(spu_context* ctx, uint32_t ea, int poison)
{
    for (unsigned i = 0; i < 64; i++) ctx->ls[0x1000 + i] = (uint8_t)(i + 1);
    if (poison) { ctx->ls[0x1020] = 0xC7; ctx->ls[0x1021] = 0x08;
                  ctx->ls[0x1022] = 0xC7; ctx->ls[0x1023] = 0x08; }
    check(mfc_do_transfer(ctx, 0x1000, ea, 64, 0x20) == 0, "PUT return");
    check(memcmp(vm_base + ea, ctx->ls + 0x1000, 64) == 0, "PUT bytes");
}

int main(int argc, char** argv)
{
    check(argc == 2, "mode argument");
#ifdef _WIN32
    puts("SKIP: needs a lazily committed 1 GiB mapping"); return 0;
#else
    /* The region sits at 0x40003000: back the guest space up to just past
     * it, committed lazily by the host. */
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
    void* m = mmap(NULL, 0x40010000u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    check(m != MAP_FAILED, "map guest space");
    vm_base = (uint8_t*)m;
    memset(g_vm_page_bitmap, 0xFF, sizeof g_vm_page_bitmap);
    spu_context* ctx = (spu_context*)calloc(1, sizeof(*ctx));
    check(ctx != NULL, "allocate context");
    spu_context_init(ctx, 0);
    ctx->image_id = 7;
    if (strcmp(argv[1], "outside") == 0) {
        put(ctx, 0x00020000u, 1);   /* poison outside the region */
        put(ctx, 0x00020100u, 0);   /* clean outside the region: never reported */
    } else if (strcmp(argv[1], "region") == 0) {
        put(ctx, 0x40003000u, 0);   /* clean, in the region */
        put(ctx, 0x40003100u, 1);   /* poison, in the region */
    } else {
        check(0, "known mode");
    }
    fflush(stderr);
    puts("OK: poison PUT report");
    return 0;
#endif
}

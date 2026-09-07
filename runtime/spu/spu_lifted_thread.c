/* spu_lifted_thread.c -- see spu_lifted_thread.h for what this is for. */

#include "spu_lifted_thread.h"
#include "spu_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t* vm_base;

/* The lifted-code registry, the dispatcher and the per-context MFC engine
 * table, all in spu_channels.c. */
int  spu_have_function(uint32_t addr);
int  spu_image_of_function(uint32_t addr);
void spu_indirect_branch(struct spu_context* ctx);
int  spu_run_with_halt(void (*entry)(struct spu_context*), struct spu_context* ctx);
void spu_mfc_release(struct spu_context* ctx);

/* Guest memory is big-endian and this file reads descriptors out of it. lv2 has
 * its own copy of this helper; duplicating four lines is cheaper than exporting
 * one from a translation unit full of syscall handlers. */
static uint32_t rd_be32(uint32_t ea)
{
    if (!vm_base) return 0;
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Lay a sys_spu_image's segments into a local store.
 *
 *   sys_spu_image   { u32 type; u32 entry_point; sys_spu_segment* segs; s32 nsegs; }
 *   sys_spu_segment { s32 type; u32 ls_start; s32 size; u64 src; }   (0x18 bytes)
 *
 * COPY (type 1) reads from the segment's source EA; FILL (type 2) writes the
 * value in the same field, which the SDK counts as a segment of its own for an
 * ELF's bss tail. INFO (type 4) is metadata and is not loaded.
 *
 * _sys_spu_image_import writes the source EA at both +0x10 and +0x14 (the
 * comment there explains why: readers disagree about whether the field is a u32
 * or the low half of a big-endian u64). Reading +0x10 first and falling back
 * keeps this working for images built by either convention. */
static void spu_deploy_image(spu_context* ctx, uint32_t img_ea)
{
    if (!img_ea || !vm_base) return;
    uint32_t segs_ea = rd_be32(img_ea + 0x08);
    int32_t  nsegs   = (int32_t)rd_be32(img_ea + 0x0C);
    if (!segs_ea) return;

    for (int32_t i = 0; i < nsegs; i++) {
        uint32_t s    = segs_ea + (uint32_t)i * 0x18u;
        uint32_t type = rd_be32(s + 0x00);
        uint32_t ls   = rd_be32(s + 0x04) & SPU_LS_MASK;
        uint32_t size = rd_be32(s + 0x08);
        uint32_t src  = rd_be32(s + 0x10);
        if (!src) src = rd_be32(s + 0x14);
        if (size > SPU_LS_SIZE - ls) size = SPU_LS_SIZE - ls;
        if (type == 1) {
            if (src) memcpy(&ctx->ls[ls], vm_base + src, size);
        } else if (type == 2) {
            memset(&ctx->ls[ls], (int)(src & 0xFFu), size);
        }
    }
}

int spu_lifted_thread_available(uint32_t entry)
{
    return spu_have_function(entry & SPU_LS_MASK);
}

void spu_lifted_thread_setup(spu_context* ctx, const spu_lifted_thread_desc* d)
{
    if (!ctx || !d) return;

    spu_context_init(ctx, d->tid);
    ctx->spu_id       = d->tid;
    ctx->spu_group_id = d->group_id;
    ctx->pc           = d->entry & SPU_LS_MASK;

    /* Start the thread in the image its entry point belongs to, rather than in
     * image 0. Zero is the registry's wildcard, so a context that keeps it will
     * happily resolve a later branch into whatever other image registered that
     * LS address -- SPURS loads its kernel, policy modules and job binaries at
     * overlapping addresses, so that is not a rare collision but the normal
     * case. Asking the registry which image owns the entry point gets the same
     * answer the title's own registration order implies, without the lv2 layer
     * having to know any image ids. */
    { int img = spu_image_of_function(ctx->pc);
      ctx->image_id = (img >= 0) ? img : 0; }

    if (d->img_ea) spu_deploy_image(ctx, d->img_ea);

    /* sys_spu_thread_argument is four u64s and lv2 puts each in the preferred
     * doubleword of r3..r6 (RPCS3 sys_spu.cpp: gpr[3+i] = from64(0, arg[i])).
     * Lane 0 is the preferred word here, so the u64 occupies lanes 0 and 1 and
     * the rest of the register is zero, which memset already did. */
    for (int a = 0; a < 4; a++) {
        ctx->gpr[3 + a]._u32[0] = (uint32_t)(d->args[a] >> 32);
        ctx->gpr[3 + a]._u32[1] = (uint32_t)d->args[a];
    }

    /* r1 is deliberately left at 0: an SPU image's own crt sets its stack up,
     * and lv2 does not. The job helpers in spu_lifted_job.h do seed it, because
     * they enter a lifted function directly rather than starting a thread. */
}

void spu_lifted_thread_run(spu_context* ctx, spu_lifted_thread_result* out)
{
    spu_lifted_thread_result r;
    memset(&r, 0, sizeof(r));
    if (!ctx) { if (out) *out = r; return; }

    fprintf(stderr, "[SPU] tid=0x%X RUNNING lifted image entry=0x%05X (image %d)\n",
            ctx->spu_id, ctx->pc, ctx->image_id);
    fflush(stderr);

    ctx->status = SPU_STATUS_RUNNING;
    /* The dispatcher resolves ctx->pc in ctx->image_id and runs it; the halt
     * landing pad catches an SPU that stops from inside a nested call, and
     * drains the tail-transfer chain when it returns normally. */
    spu_run_with_halt(spu_indirect_branch, ctx);

    /* What the SPU meant by stopping. The SPU-side sys_spu_thread_exit ABI
     * writes the status to SPU_WrOutMbox and then executes stop 0x102; lv2 pops
     * that mailbox value as the thread's exit status. The 14-bit stop code is
     * the selector, not the status (CBEA p97, SPU_Status.StopCode): 0x102
     * THREAD_EXIT carries this thread's status, 0x101 GROUP_EXIT the group's. */
    if (ctx->status == SPU_STATUS_STOPPED_BY_STOP &&
        ctx->stop_code == 0x102u && ctx->ch_out_mbox.count) {
        r.exit_status = (int32_t)ctx->ch_out_mbox.value;
        ctx->ch_out_mbox.count = 0;
    } else if (ctx->status == SPU_STATUS_STOPPED_BY_STOP &&
               ctx->stop_code == 0x101u && ctx->ch_out_mbox.count) {
        r.group_exit   = 1;
        r.group_status = (int32_t)ctx->ch_out_mbox.value;
        ctx->ch_out_mbox.count = 0;
    } else {
        /* Anything else -- a halt from the dispatcher's unresolved-branch
         * unwind, or a stop code outside the exit protocol -- is a fault. The
         * thread still exits with 0, as it does on the fallback path, but the
         * caller is told so it can report or raise an exception event. */
        r.faulted = 1;
    }

    fprintf(stderr, "[SPU] tid=0x%X stopped (status=0x%X pc=0x%05X code=0x%X) "
            "-> status=%d%s%s\n",
            ctx->spu_id, ctx->status, ctx->pc, ctx->stop_code, r.exit_status,
            r.group_exit ? " group-exit" : "", r.faulted ? " FAULT" : "");
    fflush(stderr);

    /* The SPU has stopped, so its MFC engine is idle and the slot it holds in
     * the per-context table can go back. There are eight of those and they used
     * to be claimed for good, which a title only notices once it has started
     * its ninth SPU thread: from there on every thread shares one engine with
     * every other, tags and queue included. This is the natural place to give
     * one back -- the context may be started again (the group is destroyed
     * later, and lv2 keeps the context until then), and a fresh run claims a
     * freshly initialized engine, which is what a fresh SPU thread should get.
     *
     * The fallback and interpreter paths are unchanged: neither runs a context
     * through here, so both keep the engine they had for as long as they had
     * it. */
    spu_mfc_release(ctx);

    if (out) *out = r;
}

/* spu_intr.inc -- raw-SPU class-2 interrupts delivered to a PPU handler thread.
 *
 * ps1_netemu drives its five raw SPUs entirely through interrupts. Setup is:
 *
 *     sc 150(spu, class=2, .., &tag)   sys_raw_spu_create_interrupt_tag
 *     sc  84(&ih, tag, thread, arg)    sys_interrupt_thread_establish
 *     sc 151(spu, class=2, mask=3)     sys_raw_spu_set_int_mask
 *
 * observed as spu0 -> tag 0x52000002 -> thread 5 (gpuCmdInterruptHandlerThread)
 * and spu4 -> tag 0x52000402 -> thread 7 (SPUCxInterruptHandlerThread).
 *
 * With 84 and 88 left as stubs the handshake was one-way: each SPU wrote its
 * outbound mailbox, the PPU read all five once while it was still polling, and
 * then waited to be interrupted. It never was, so it never replied -- SPUs 0-3
 * parked on SPU_RdInMbox forever and SPU 4 gave up and exited with status 0.
 *
 * The handler body ends in `blr` after its `sc 88`, so eoi is NOT meant to
 * return into the guest: lv2 re-enters the thread at its ENTRY on the next
 * interrupt. That is why eoi here simply returns -- the body then falls out
 * naturally and ppu_host_thread_proc re-invokes the entry once the next
 * interrupt arrives.
 *
 * ponytail: a flag plus a 200us poll rather than a condition variable, so the
 * whole mechanism stays inside this file with no cross-module plumbing. The
 * ceiling is 200us of interrupt latency; swap in a CV if a title ever needs
 * tighter than that.
 */

#define PS3_INTR_MAX 8

typedef struct {
    uint32_t tag;
    uint64_t thread;
    volatile long pending;
    int       used;
} ps3_intr_bind;

static ps3_intr_bind s_intr[PS3_INTR_MAX];

/* Bind an interrupt tag to the PPU thread that services it. */
int ps3_intr_establish(uint32_t tag, uint64_t thread)
{
    for (int i = 0; i < PS3_INTR_MAX; i++) {
        if (s_intr[i].used && s_intr[i].tag == tag) {
            s_intr[i].thread = thread;
            return 0;
        }
    }
    for (int i = 0; i < PS3_INTR_MAX; i++) {
        if (!s_intr[i].used) {
            s_intr[i].used = 1;
            s_intr[i].tag = tag;
            s_intr[i].thread = thread;
            s_intr[i].pending = 0;
            fprintf(stderr, "[intr] tag 0x%08X -> ppu thread %llu\n",
                    tag, (unsigned long long)thread);
            return 0;
        }
    }
    return -1;
}

/* Is this PPU thread an established interrupt handler? */
int ps3_intr_is_handler(uint64_t thread)
{
    for (int i = 0; i < PS3_INTR_MAX; i++)
        if (s_intr[i].used && s_intr[i].thread == thread) return 1;
    return 0;
}

/* Raise the interrupt for a tag. Called from the SPU side. */
static void ps3_intr_raise(uint32_t tag)
{
    for (int i = 0; i < PS3_INTR_MAX; i++) {
        if (s_intr[i].used && s_intr[i].tag == tag) {
#ifdef _WIN32
            InterlockedExchange(&s_intr[i].pending, 1);
#else
            s_intr[i].pending = 1;
#endif
            return;
        }
    }
}

/* Is an interrupt actually asserted for this tag right now?
 *
 * LEVEL-triggered, deliberately. The tag encodes the SPU
 * (0x52000000 | id<<8 | class), and the real hardware condition is simply
 * "int_stat & int_mask" -- the PPU clears it explicitly with
 * sys_raw_spu_set_int_stat (153, write-1-to-clear). An edge-triggered flag
 * loses the interrupt raised BEFORE the handler is established, which is
 * exactly what happens here: all five SPUs write their ready mailbox during
 * init, and the two handlers are established afterwards. Reading the level
 * cannot lose that. */
static int ps3_intr_level(uint32_t tag)
{
    unsigned n = (tag >> 8) & 0xFFu;
    if (n >= SPU_RAW_COUNT) return 0;
    if (!s_spu[n].used) return 0;
    return (s_spu[n].int_stat & s_spu[n].int_mask) != 0;
}

/* Block this handler thread until its interrupt is asserted. Returns 0 if the
 * thread is not a handler at all (the caller then lets it finish normally). */
int ps3_intr_wait(uint64_t thread)
{
    ps3_intr_bind* b = 0;
    for (int i = 0; i < PS3_INTR_MAX; i++)
        if (s_intr[i].used && s_intr[i].thread == thread) { b = &s_intr[i]; break; }
    if (!b) return 0;
    for (;;) {
        if (ps3_intr_level(b->tag)) {
            static unsigned long _n = 0;
            if (++_n <= 8)
                fprintf(stderr, "[intr] deliver tag 0x%08X -> thread %llu\n",
                        b->tag, (unsigned long long)thread);
            return 1;
        }
#ifdef _WIN32
        Sleep(1);
#else
        { struct timespec ts = {0, 200 * 1000}; nanosleep(&ts, 0); }
#endif
    }
}

/* 84: sys_interrupt_thread_establish(u64* ih, u32 intrtag, u64 intrthread, u64 arg) */
static int64_t sc_interrupt_thread_establish(ppu_context* ctx)
{
    uint32_t ih_ea = (uint32_t)ctx->gpr[3];
    uint32_t tag   = (uint32_t)ctx->gpr[4];
    uint64_t thr   = ctx->gpr[5];
    if (ps3_intr_establish(tag, thr) != 0) return CELL_EAGAIN;
    /* The handle is opaque to the guest; it only ever passes it back to
     * sys_interrupt_thread_disestablish. The tag identifies it uniquely. */
    if (ih_ea) { be32_store(ih_ea, 0); be32_store(ih_ea + 4, tag); }
    return CELL_OK;
}

/* 89: _sys_interrupt_thread_disestablish(handle, u64* status) -- unbind. The
 * handle we handed back in establish is the tag. Returning ESRCH from the stub
 * made ps1_netemu print "sys_interrupt_thread_disestablish() failed" twice
 * during R3000 teardown. */
static int64_t sc_interrupt_thread_disestablish(ppu_context* ctx)
{
    uint32_t tag    = (uint32_t)ctx->gpr[3];
    uint32_t out_ea = (uint32_t)ctx->gpr[4];
    for (int i = 0; i < PS3_INTR_MAX; i++)
        if (s_intr[i].used && s_intr[i].tag == tag) { s_intr[i].used = 0; break; }
    if (out_ea) { be32_store(out_ea, 0); be32_store(out_ea + 4, 0); }
    return CELL_OK;
}

/* 88: sys_interrupt_thread_eoi() -- end this pass. See the note above on why
 * returning is right here even though lv2's own eoi does not return. */
static int64_t sc_interrupt_thread_eoi(ppu_context* ctx)
{
    (void)ctx;
    return CELL_OK;
}

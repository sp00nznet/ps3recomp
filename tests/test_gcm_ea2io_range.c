/* gcm_ea2io_range(), the EA -> IO lookup behind every SPU PUT, stops at the
 * mapping table's high-water mark instead of walking all of it. Checks that
 * it still returns what a walk of the whole table returns -- the first active
 * mapping in slot order and its remaining room -- across maps, unmaps, holes
 * and overlapping ranges, and that every active slot is below the mark.
 * Includes the real implementation, as test_gcm_map_flags.c does. */
#include "../libs/video/cellGcmSys.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
int g_resv_store_active;
void ppu_resv_break_store(uint64_t addr) { (void)addr; abort(); }
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t addr) { (void)addr; abort(); }
void spu_lockline_lock(void) { abort(); }
void spu_lockline_unlock(void) { abort(); }

/* The lookup over every slot: what gcm_ea2io_range returned before the bound. */
static u32 full_scan(u32 ea, u32* room)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        const IoMapping* m = &s_io_mappings[i];
        if (!m->active || !m->size) continue;
        if (ea >= m->ea && ea < m->ea + m->size) {
            if (room) *room = m->ea + m->size - ea;
            return m->io + (ea - m->ea);
        }
    }
    return 0xFFFFFFFFu;
}

static int g_checks;
static void agree(u32 ea)
{
    u32 room_a = 0xA5A5A5A5u, room_b = 0xA5A5A5A5u;
    const u32 a = gcm_ea2io_range(ea, &room_a);
    const u32 b = full_scan(ea, &room_b);
    if (a != b || room_a != room_b) {
        fprintf(stderr, "FAIL ea 0x%08X: bounded io 0x%08X room 0x%X, full io 0x%08X room 0x%X\n",
                ea, a, room_a, b, room_b);
        abort();
    }
    g_checks++;
}

static void below_mark(void)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++)
        if (s_io_mappings[i].active) assert(i < s_io_mapping_hw);
}

static u32 g_rng = 0x2545F491u;
static u32 rnd(u32 n) { g_rng = g_rng * 1664525u + 1013904223u; return (g_rng >> 8) % n; }

#define EA0 0x40000000u
static void probe_all(void)
{
    /* Every 64 KB through the window the mappings live in, plus the edges. */
    for (u32 ea = EA0 - 0x100000u; ea < EA0 + 0x1000000u; ea += 0x10000u) {
        agree(ea); agree(ea + 0xFFFFu);
    }
    agree(0); agree(0xFFFFFFFFu);
    below_mark();
}

int main(void)
{
    vm_base = calloc(1, 0x10000); assert(vm_base);
    /* stdout carries the mapping log lines; the verdict goes to stderr. */
    if (!freopen("/dev/null", "w", stdout)) return 1;

    /* An empty table: nothing maps and nothing is visited. */
    assert(s_io_mapping_hw == 0);
    assert(gcm_ea2io_range(EA0, NULL) == 0xFFFFFFFFu);

    /* First match in slot order: two overlapping ranges, the earlier slot
     * wins; unmapping it hands the address to the later one; a new mapping
     * that lands in the hole wins again. */
    cellGcm_syscall_iomap(EA0, 0x00100000u, 0x200000u);            /* slot 0 */
    cellGcm_syscall_iomap(EA0 + 0x100000u, 0x00800000u, 0x200000u); /* slot 1 */
    assert(gcm_ea2io_range(EA0 + 0x180000u, NULL) == 0x00280000u);
    assert(cellGcmUnmapEaIoAddress(EA0) == CELL_OK);
    assert(gcm_ea2io_range(EA0 + 0x180000u, NULL) == 0x00880000u);
    cellGcm_syscall_iomap(EA0 + 0x100000u, 0x00C00000u, 0x100000u); /* slot 0 again */
    assert(s_io_mappings[0].active && s_io_mappings[0].io == 0x00C00000u);
    assert(gcm_ea2io_range(EA0 + 0x180000u, NULL) == 0x00C80000u);
    assert(s_io_mapping_hw == 2);
    probe_all();

    /* Random maps and unmaps over a small window, so ranges overlap and the
     * table keeps gaining and losing holes. */
    for (int step = 0; step < 4000; step++) {
        if (s_io_mapping_count && rnd(3) == 0) {
            int k = (int)rnd((u32)s_io_mapping_count);
            for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++)
                if (s_io_mappings[i].active && k-- == 0) {
                    assert(cellGcmUnmapIoAddress(s_io_mappings[i].io) == CELL_OK);
                    break;
                }
        } else if (s_io_mapping_count < 48) {
            cellGcm_syscall_iomap(EA0 + rnd(16) * 0x100000u, rnd(200) * 0x100000u,
                                  (1u + rnd(3)) * 0x100000u);
        }
        if (step % 50 == 0) probe_all();
    }
    probe_all();
    assert(s_io_mapping_hw <= 48 + 1);

    /* Terminating clears the mark with the table. */
    assert(s_io_mapping_hw > 2);
    cellGcmTerminate();
    assert(s_io_mapping_hw == 0 && s_io_mapping_count == 0);
    probe_all();

    fprintf(stderr, "PASS: bounded EA->IO lookup matched the full-table walk in %d probes\n",
            g_checks);
    return 0;
}

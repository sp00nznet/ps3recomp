/* cellAudioAdd2chData/AddData/Add6chData mix one block of big-endian float
 * PCM into the next block the mixer has not consumed, consecutive adds fill
 * consecutive blocks, an add that would overrun the ring is dropped; and
 * cellAudioSetNotifyEventQueue with a key that is already registered does not
 * register it a second time. No device or mixer thread is started.
 * Run: python tools/test_audio_add_data.py */
#include "../cellAudio.c"

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
/* lv2 event queues and HLE registration: not exercised here. */
int sys_event_queue_push_by_id(uint32_t queue_id, uint64_t source, uint64_t d1, uint64_t d2, uint64_t d3)
{ (void)queue_id; (void)source; (void)d1; (void)d2; (void)d3; return 0; }
uint32_t sys_event_find_queue_by_key(uint64_t key) { (void)key; return 0; }
uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size) { (void)key; (void)size; return 0; }
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*)) { (void)nid; (void)name; (void)fn; }

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

static void put_be_f32(u32 ea, float v)
{
    u32 w; memcpy(&w, &v, 4);
    vm_base[ea] = (u8)(w >> 24); vm_base[ea + 1] = (u8)(w >> 16);
    vm_base[ea + 2] = (u8)(w >> 8); vm_base[ea + 3] = (u8)w;
}

/* Sample s, channel c of ring block b of port 0, read back from big-endian. */
static float ring(u32 b, u32 s, u32 c)
{
    const AudioPortSlot* p = &s_ports[0];
    const u32 nch = (u32)p->param.nChannel;
    return ld_be_f32(&p->buffer[(b * CELL_AUDIO_BLOCK_SAMPLES + s) * nch + c]);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 0x10000);
    if (!vm_base) return 2;
    mutex_init(&s_audio_mutex);

    /* Not initialised yet. */
    CHECK(cellAudioAdd2chData(0, 0x1000, CELL_AUDIO_BLOCK_SAMPLES, 1.0f) == CELL_AUDIO_ERROR_NOT_INIT);
    s_audio_initialized = 1;

    /* A closed port. */
    CHECK(cellAudioAdd2chData(0, 0x1000, CELL_AUDIO_BLOCK_SAMPLES, 1.0f) == CELL_AUDIO_ERROR_PORT_NOT_OPEN);

    const u32 blocks = 8;
    AudioPortSlot* port = &s_ports[0];
    memset(port, 0, sizeof(*port));
    port->in_use = 1;
    port->param.nChannel = 8;
    port->param.nBlock = blocks;
    port->buffer = (float*)calloc(blocks * CELL_AUDIO_BLOCK_SAMPLES * 8, sizeof(float));
    if (!port->buffer) return 2;

    /* One stereo block: L = 0.5, R = -0.25 for every sample. */
    for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
        put_be_f32(0x1000 + s * 8, 0.5f);
        put_be_f32(0x1000 + s * 8 + 4, -0.25f);
    }

    CHECK(cellAudioAdd2chData(0, 0x1000, 128, 1.0f) == CELL_AUDIO_ERROR_PARAM);   /* not one block */
    CHECK(cellAudioAdd2chData(0, 0, CELL_AUDIO_BLOCK_SAMPLES, 1.0f) == CELL_AUDIO_ERROR_PARAM);

    /* First add lands one block ahead of the mixer (read_index 0 -> block 1),
     * scaled by the volume, into channels 0 and 1 only. */
    CHECK(cellAudioAdd2chData(0, 0x1000, CELL_AUDIO_BLOCK_SAMPLES, 2.0f) == CELL_OK);
    CHECK(ring(1, 0, 0) == 1.0f && ring(1, 255, 0) == 1.0f);
    CHECK(ring(1, 0, 1) == -0.5f);
    CHECK(ring(1, 0, 2) == 0.0f && ring(1, 0, 7) == 0.0f);
    CHECK(ring(0, 0, 0) == 0.0f && ring(2, 0, 0) == 0.0f);

    /* The next add fills the next block, and adds mix rather than overwrite. */
    CHECK(cellAudioAdd2chData(0, 0x1000, CELL_AUDIO_BLOCK_SAMPLES, 1.0f) == CELL_OK);
    CHECK(ring(2, 0, 0) == 0.5f);
    CHECK(ring(1, 0, 0) == 1.0f);

    /* Blocks 3..7 fill the ring; the add after that would overrun it and is
     * dropped (still CELL_OK), leaving block 0 -- the one the mixer is on -- alone. */
    for (int i = 0; i < 5; i++)
        CHECK(cellAudioAdd2chData(0, 0x1000, CELL_AUDIO_BLOCK_SAMPLES, 1.0f) == CELL_OK);
    CHECK(ring(7, 0, 0) == 0.5f);
    CHECK(port->add_block == 8);
    CHECK(cellAudioAdd2chData(0, 0x1000, CELL_AUDIO_BLOCK_SAMPLES, 1.0f) == CELL_OK);
    CHECK(port->add_block == 8);
    CHECK(ring(0, 0, 0) == 0.0f);

    /* Once the mixer has moved on, the cursor follows it. */
    port->read_index = 5;
    CHECK(cellAudioAddData(0, 0x1000, CELL_AUDIO_BLOCK_SAMPLES, 1.0f) == CELL_OK);   /* nChannel = 8 */
    CHECK(port->add_block == 9);

    /* Create-then-Set with the same key registers it once. */
    CHECK(cellAudioSetNotifyEventQueue(0x1234) == CELL_OK);
    CHECK(cellAudioSetNotifyEventQueue(0x1234) == CELL_OK);
    int slots = 0;
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++)
        slots += s_notify_queues[i].in_use && s_notify_queues[i].key == 0x1234;
    CHECK(slots == 1);

    free(port->buffer);
    free(vm_base);
    if (failures) return 1;
    puts("PASS: cellAudioAdd*Data fill consecutive blocks ahead of the mixer; one notify slot per key");
    return 0;
}

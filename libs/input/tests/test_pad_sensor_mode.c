/* cellPadInfoSensorMode / cellPadSetSensorMode: they agree with the SENSOR_MODE
 * capability the virtual pad advertises, refuse a closed library or a bad
 * port, and Set toggles CELL_PAD_SETTING_SENSOR_ON in the port setting without
 * touching press mode; with it on, cellPadGetData's packet carries the sensor
 * block at its at-rest values.
 *
 *   clang -std=gnu17 -I include $(sdl2-config --cflags) -I "$(sdl2-config --prefix)/include" \
 *         libs/input/tests/test_pad_sensor_mode.c $(sdl2-config --libs) \
 *         -o /tmp/test_pad_sensor_mode && /tmp/test_pad_sensor_mode
 */
#include "../cellPad.c"
#include <assert.h>
#include <sys/mman.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
u32 cellGcm_flip_request_count(void) { return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

/* button[k] of the CellPadData cellPadGetData wrote at `ea` (big-endian). */
static unsigned pad_button(u32 ea, int k)
{
    return ((unsigned)vm_base[ea + 4u + 2u * (u32)k] << 8) | vm_base[ea + 5u + 2u * (u32)k];
}

int main(void)
{
    setenv("PS3_PAD_SDL", "0", 1);
    setenv("PAD_ALWAYS_CHANGE", "1", 1);   /* a packet on every read */
    vm_base = mmap(NULL, 1u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);

    assert(cellPadInfoSensorMode(0) == CELL_PAD_ERROR_NOT_OPENED);
    assert(cellPadSetSensorMode(0, 1) == CELL_PAD_ERROR_NOT_OPENED);

    assert(cellPadInit(1) == CELL_OK);
    assert(cellPadInfoSensorMode(CELL_PAD_MAX_PORT_NUM) == CELL_PAD_ERROR_INVALID_PARAMETER);
    assert(cellPadSetSensorMode(CELL_PAD_MAX_PORT_NUM, 1) == CELL_PAD_ERROR_INVALID_PARAMETER);

    /* The capability word claims the sensors, so Info must too. */
    assert(cellPadGetCapabilityInfo(0, (CellPadCapabilityInfo*)(uintptr_t)0x1000) == CELL_OK);
    assert(vm_read32(0x1000) & CELL_PAD_CAPABILITY_SENSOR_MODE);
    assert(cellPadInfoSensorMode(0) == 1);

    assert(cellPadSetPressMode(0, 1) == CELL_OK);
    assert(cellPadSetSensorMode(0, 1) == CELL_OK);
    assert(s_port_setting[0] == (CELL_PAD_SETTING_PRESS_ON | CELL_PAD_SETTING_SENSOR_ON));

    /* The packet now carries the sensor block: a DS3 lying still. */
    assert(cellPadGetData(0, (CellPadData*)(uintptr_t)0x2000) == CELL_OK);
    assert(vm_read32(0x2000) == CELL_PAD_LEN_CHANGE_SENSOR_ON);
    assert(pad_button(0x2000, CELL_PAD_BTN_OFFSET_SENSOR_X) == 512);
    assert(pad_button(0x2000, CELL_PAD_BTN_OFFSET_SENSOR_Y) == 399);
    assert(pad_button(0x2000, CELL_PAD_BTN_OFFSET_SENSOR_Z) == 512);
    assert(pad_button(0x2000, CELL_PAD_BTN_OFFSET_SENSOR_G) == 512);

    assert(cellPadSetSensorMode(0, 0) == CELL_OK);
    assert(s_port_setting[0] == CELL_PAD_SETTING_PRESS_ON);
    assert(cellPadGetData(0, (CellPadData*)(uintptr_t)0x2000) == CELL_OK);
    assert(vm_read32(0x2000) == CELL_PAD_LEN_CHANGE_PRESS_ON);   /* back to press mode's */

    assert(cellPadEnd() == CELL_OK);
    puts("cellPad sensor mode checks passed");
    return 0;
}

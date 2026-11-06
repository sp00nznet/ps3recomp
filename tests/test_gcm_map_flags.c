/* Include the real implementation to inspect both host tables and mapping
 * slots; no copy of the mapping algorithm or production-only test API. */
#include "../libs/video/cellGcmSys.c"
#include "../libs/video/rsx_tiling.c"
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

static void reset_maps(void) {
    memset(s_io_mappings,0,sizeof(s_io_mappings));s_io_mapping_count=0;
    memset(s_io_address_table,0xff,sizeof(s_io_address_table));
    memset(s_ea_address_table,0xff,sizeof(s_ea_address_table));
    memset(&s_config,0,sizeof(s_config));
}
static void reject(u32 ea,u32 io,u32 size,u32 flags,s32 expected) {
    IoMapping mappings[CELL_GCM_MAX_IO_MAPPINGS];
    u16 forward[65536],reverse[65536];
    memcpy(mappings,s_io_mappings,sizeof(mappings));
    memcpy(forward,s_io_address_table,sizeof(forward));
    memcpy(reverse,s_ea_address_table,sizeof(reverse));
    int count=s_io_mapping_count;
    assert(cellGcmMapEaIoAddressWithFlags(ea,io,size,flags)==expected);
    assert(count==s_io_mapping_count);
    assert(!memcmp(mappings,s_io_mappings,sizeof(mappings)));
    assert(!memcmp(forward,s_io_address_table,sizeof(forward)));
    assert(!memcmp(reverse,s_ea_address_table,sizeof(reverse)));
}
int main(void) {
    vm_base=calloc(1,0x10000);assert(vm_base);reset_maps();
    const u32 strict=2, page=0x100000;
    assert(cellGcmMapEaIoAddressWithFlags(0x44e00000,0x0e000000,2*page,strict)==CELL_OK);
    const u32 offsets[]={0,0xfffff,0x100000,0x1fffff};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        u32 delta=offsets[i];
        assert(cellGcmAddressToOffset(0x44e00000+delta,(u32*)(uintptr_t)0x100)==CELL_OK);
        assert(vm_read32(0x100)==0x0e000000+delta);
        assert(cellGcmIoOffsetToAddress(0x0e000000+delta,(u32*)(uintptr_t)0x104)==CELL_OK);
        assert(vm_read32(0x104)==0x44e00000+delta);
        assert(gcm_io2ea(0x0e000000+delta)==0x44e00000+delta);
    }
    ppu_hle_inject_base=0x1000;
    assert(cellGcmGetOffsetTable((CellGcmOffsetTable*)(uintptr_t)0x200)==CELL_OK);
    assert(vm_read16(vm_read32(0x200)+0x44e*2)==0xe0);
    assert(vm_read16(vm_read32(0x204)+0xe1*2)==0x44f);
    const u32 bad_flags[]={0,1,3,4,0xffffffff};
    for(unsigned i=0;i<sizeof(bad_flags)/sizeof(bad_flags[0]);i++)
        reject(0x45000000,0x0e200000,page,bad_flags[i],(s32)CELL_GCM_ERROR_INVALID_VALUE);
    reject(0x45000001,0x0e200000,page,strict,(s32)CELL_GCM_ERROR_INVALID_ALIGNMENT);
    reject(0x45000000,0x0e200001,page,strict,(s32)CELL_GCM_ERROR_INVALID_ALIGNMENT);
    reject(0x45000000,0x0e200000,0,strict,(s32)CELL_GCM_ERROR_INVALID_ALIGNMENT);
    reject(0x45000000,0x0e200000,page+1,strict,(s32)CELL_GCM_ERROR_INVALID_ALIGNMENT);
    reject(0xfff00000,0,2*page,strict,(s32)CELL_GCM_ERROR_INVALID_VALUE);
    reject(0x45000000,0x0ff00000,2*page,strict,(s32)CELL_GCM_ERROR_INVALID_VALUE);
    reject(0x45000000,0xfff00000,2*page,strict,(s32)CELL_GCM_ERROR_INVALID_VALUE);
    /* Overlap at either edge, from inside, or enclosing the whole old map. */
    reject(0x44f00000,0x01000000,page,strict,(s32)CELL_GCM_ERROR_ADDRESS_OVERWRAP);
    reject(0x44d00000,0x01000000,2*page,strict,(s32)CELL_GCM_ERROR_ADDRESS_OVERWRAP);
    reject(0x44d00000,0x01000000,4*page,strict,(s32)CELL_GCM_ERROR_ADDRESS_OVERWRAP);
    reject(0x45000000,0x0e100000,page,strict,(s32)CELL_GCM_ERROR_ADDRESS_OVERWRAP);
    reject(0x45000000,0x0df00000,4*page,strict,(s32)CELL_GCM_ERROR_ADDRESS_OVERWRAP);
    /* Adjacent ranges are valid, including the last page of the IO aperture. */
    assert(cellGcmMapEaIoAddressWithFlags(0x45000000,0x0e200000,page,strict)==CELL_OK);
    assert(cellGcmMapEaIoAddressWithFlags(0x45100000,0x0ff00000,page,strict)==CELL_OK);
    assert(cellGcmUnmapEaIoAddress(0x44e00000)==CELL_OK);
    assert(s_io_address_table[0x44e]==0xffff && s_io_address_table[0x44f]==0xffff);
    assert(s_ea_address_table[0xe0]==0xffff && s_ea_address_table[0xe1]==0xffff);
    assert(cellGcmMapEaIoAddressWithFlags(0x44e00000,0x0e000000,2*page,strict)==CELL_OK);
    reset_maps();
    for(unsigned i=0;i<CELL_GCM_MAX_IO_MAPPINGS;i++)
        assert(cellGcmMapEaIoAddress(0x10000000+i*page,i*page,page)==CELL_OK);
    reject(0x20000000,0x08000000,page,strict,(s32)CELL_GCM_ERROR_FAILURE);
    free(vm_base);
    puts("PASS: flagged IO translation, table publication, unmap, rejection rollback and capacity");
}

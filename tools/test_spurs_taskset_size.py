#!/usr/bin/env python3
"""Run the real HLE attribute and taskset creation path against a synthetic VM."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: standalone fixture requires POSIX C toolchain')
    raise SystemExit(0)
FIXTURE = r'''
#include "libs/spurs/cellSpurs.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
/* No SPU runs in this layout fixture; observer-only services are inert. */
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
int spu_coh_is_reserved(uint32_t ea) {(void)ea; return 0;}
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t ea,uint64_t value,int width) {(void)ea;(void)value;(void)width;}
void spu_taskset2_register(uint32_t ea) {(void)ea;}
void spu_taskset_register(uint32_t ea) {(void)ea;}
int64_t sys_event_flag_create_host(uint32_t id_out, uint64_t init) {
    vm_write32(id_out, 7u); (void)init; return 0; }
static _Alignas(128) uint8_t memory[0x10000];
#define PTR(type, ea) ((type*)(uintptr_t)(ea))
static void expect(int ok, const char* what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); abort(); }
}
static void clean(uint32_t size) {
    for (uint32_t i=0;i<size;i++)
        if (i<0x60 || (i>=0x80 && i<0x1890) || i>=0x18A0)
            expect(memory[0x4000+i]==0,"requested storage is cleared");
    expect(memory[0x3FFF]==0xA5 && memory[0x4000+size]==0xA5,
           "taskset creation preserves both canaries");
}
int main(void) {
    vm_base=memory; ppu_vm_size=sizeof(memory); s_inst[0].ea=0x1000;
    CellSpursTasksetAttribute* attr=PTR(CellSpursTasksetAttribute,0x2000);
    CellSpursTaskset* ts=PTR(CellSpursTaskset,0x4000);
    CellSpurs* spurs=PTR(CellSpurs,0x1000);
    memset(memory,0xA5,sizeof(memory));
    expect(_cellSpursTasksetAttributeInitialize(attr,1,0,0,0,0)==0,"attribute init");
    expect(cellSpursTasksetAttributeSetTasksetSize(attr,0x2900)==0,"size request");
    expect(cellSpursCreateTasksetWithAttribute(spurs,ts,attr)==0,"extended creation");
    expect(vm_read32(0x4000+CSTS_SIZE_FIELD)==0x2900,"requested size retained in guest header");
    clean(0x2900);
    expect((uint32_t)cellSpursTasksetAttributeSetTasksetSize(NULL,0)==0x80410911u,"null precedence");
    expect((uint32_t)cellSpursTasksetAttributeSetTasksetSize(PTR(CellSpursTasksetAttribute,0x2001),0)==0x80410910u,"alignment precedence");
    expect((uint32_t)cellSpursTasksetAttributeSetTasksetSize(attr,0x2000)==0x80410902u,"unsupported size");
    expect(cellSpursCreateTasksetWithAttribute(spurs,ts,attr)==0 && vm_read32(0x4000+CSTS_SIZE_FIELD)==0x2900,"failed setter preserves size");
    expect((uint32_t)cellSpursTasksetAttributeSetTasksetSize(PTR(CellSpursTasksetAttribute,0x10000),0x2000)==0x80410902u,"invalid size precedes backing");
    expect((uint32_t)cellSpursTasksetAttributeSetTasksetSize(PTR(CellSpursTasksetAttribute,0x10000),0x2900)==0x8041090Du,"unbacked attribute setter rejected");
    expect((uint32_t)cellSpursCreateTasksetWithAttribute(spurs,ts,PTR(CellSpursTasksetAttribute,0xFFF8))==0x8041090Du,"partial attribute read rejected");
    expect((uint32_t)cellSpursCreateTasksetWithAttribute(spurs,ts,PTR(CellSpursTasksetAttribute,0x2001))==0x80410910u,"creation attribute alignment");
    ppu_vm_size=0x6800;
    memset(memory+0x4000,0xA5,0x2900);
    expect((uint32_t)cellSpursCreateTasksetWithAttribute(spurs,ts,attr)==0x8041090Du,"partial taskset backing rejected");
    for (uint32_t i=0;i<0x2900;i++) expect(memory[0x4000+i]==0xA5,"backing failure has no taskset writes");
    ppu_vm_size=sizeof(memory);
    memset(memory+0x3FFF,0xA5,0x2902);
    expect(cellSpursTasksetAttributeInitialize(attr)==0,"public attribute reinit");
    expect(cellSpursCreateTasksetWithAttribute(spurs,ts,attr)==0,"default attribute creation");
    expect(vm_read32(0x4000+CSTS_SIZE_FIELD)==0x1900,"reinit restores default size");
    clean(0x1900);
    memset(memory+0x3FFF,0xA5,0x2902);
    expect(cellSpursCreateTaskset(spurs,ts,0,NULL,0)==0,"plain creation");
    expect(vm_read32(0x4000+CSTS_SIZE_FIELD)==0x1900,"plain creation uses full default guest size");
    clean(0x1900);
    memset(memory+0x3FFF,0xA5,0x2902);
    expect(_cellSpursTasksetAttribute2Initialize(0x2000,1)==0,"attribute2 init");
    expect(vm_read32(0x2000)==1 && memory[0x2010]==1 && memory[0x2017]==1 &&
           vm_read32(0x2018)==8,"attribute2 defaults");
    expect(cellSpursCreateTaskset2(spurs,ts,0x2000)==0,"taskset2 creation");
    expect(vm_read32(0x4000+CSTS_SIZE_FIELD)==0x2900,"taskset2 uses the extended size");
    expect(vm_read32(0x4000+CSTS_EVENT_FLAG_ID1)==7 && vm_read32(0x4000+CSTS_EVENT_FLAG_ID2)==7,
           "taskset2 carries its two event flags");
    clean(0x2900);
    expect((uint32_t)cellSpursCreateTaskset2(spurs,PTR(CellSpursTaskset,0x4040),0)==
           (uint32_t)CELL_SPURS_TASK_ERROR_ALIGN,"taskset2 alignment");
    puts("PASS: real taskset requested-size retention and initialization");
}
'''
with tempfile.TemporaryDirectory(prefix='spurs-taskset-size-') as name:
    tmp=Path(name); fixture=tmp/'fixture.c'; fixture.write_text(FIXTURE)
    binary=tmp/'fixture'
    linker=['-Wl,-dead_strip'] if platform.system()=='Darwin' else ['-Wl,--gc-sections']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections','-I'+str(ROOT),'-I'+str(ROOT/'include'),
        '-I'+str(ROOT/'runtime/spu'),str(fixture),str(ROOT/'libs/spurs/spurs_taskset.c'),
        *linker,'-pthread','-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10,cwd=tmp)

/* Manufactured mapper/HLE-heap ownership regression; no title code or data. */
#include "../runtime/syscalls/sys_memory.h"
#include "../runtime/syscalls/sys_vm.h"
#include "../libs/system/sysPrxForUser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t ppu_hle_inject_base=0x70000000u;
static lv2_syscall_table table;
static unsigned checks;
/* The HLE heap uses the loader's big-endian guest accessors. */
static uint32_t vm_read32(uint32_t a) {
    uint8_t* p=vm_base+a;return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void vm_write32(uint32_t a,uint32_t v) {
    uint8_t* p=vm_base+a;p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;
}
extern int64_t sys_mmapper_allocate_memory(ppu_context*);
/* The old _sys_heap_delete_heap forwarded to this, which only knows the ids of
 * the s_heaps table and so answers ESRCH for a _sys_heap_create_heap id. */
s32 sys_heap_destroy_heap(sys_heap_t heap) { (void)heap; return (s32)CELL_ESRCH; }
static void slot_lock(void) {}
static void slot_unlock(void) {}
/* Exact production heap definitions/functions, copied without modification by
 * the driver. Only the platform lock is stubbed for this single-thread test. */
#include "ps3emu/log_first.h"   /* the copied heap code logs through it */
#include <heap-under-test.inc>
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static uint32_t out(void) { return vm_read32(0x10000); }
static int64_t reserve(uint64_t size,uint64_t alignment) {
    ppu_context c={0};c.gpr[3]=size;c.gpr[5]=alignment;c.gpr[6]=0x10000;
    return sys_mmapper_allocate_address(&c);
}
static int64_t direct(uint32_t size) {
    ppu_context c={0};c.gpr[3]=0x10000;c.gpr[4]=size;
    return sys_mmapper_allocate_memory(&c);
}
static int disjoint(uint32_t address,uint64_t size) {
    const uint64_t ranges[][2]={{YZ_HEAP_BASE,YZ_HEAP_END},
        {VM_AUDIO_BASE,(uint64_t)VM_AUDIO_BASE+VM_AUDIO_SIZE},
        {SYS_VM_REGION_BASE,SYS_VM_REGION_END}};
    for (unsigned i=0;i<sizeof(ranges)/sizeof(ranges[0]);++i)
        if (address<ranges[i][1] && ranges[i][0]<(uint64_t)address+size) return 0;
    return 1;
}
static void reset(void) {
    sys_memory_init(&table);sys_vm_init(&table);s_heap_bump=0;memset(s_heap_bin,0,sizeof(s_heap_bin));
}
int main(void) {
    CHECK(vm_init()==CELL_OK);CHECK(vm_commit(YZ_HEAP_BASE,0x100000)==CELL_OK);reset();
    CHECK(reserve(0x10000000,0x10000000)==CELL_OK);CHECK(out()==0x40000000);
    uint32_t object=(uint32_t)(uintptr_t)sys_heap_malloc(1,1024);
    CHECK(object>=YZ_HEAP_BASE && object+1024<=YZ_HEAP_END);
    memset(vm_base+object,0x5A,1024);
    CHECK(reserve(35u*1024u*1024u,0x100000)==CELL_OK);uint32_t mapped=out();
    CHECK(disjoint(mapped,35u*1024u*1024u));
    memset(vm_base+mapped,0,35u*1024u*1024u);
    CHECK(vm_read32(object)==0x5A5A5A5A);CHECK(vm_read32(object-12)==YZ_HEAP_MAGIC);
    uint32_t claim=0;
    CHECK(sys_mmapper_search_map_address(mapped,0x100000,&claim)==CELL_OK);
    CHECK(claim==mapped&&disjoint(claim,0x100000));
    claim=0xDEADBEEF;
    CHECK(sys_mmapper_search_map_address(YZ_HEAP_BASE,0x100000,&claim)!=(int64_t)CELL_OK);
    CHECK(claim==0xDEADBEEF);
    sys_heap_free(1,(void*)(uintptr_t)object);
    CHECK((uint32_t)(uintptr_t)sys_heap_malloc(1,1024)==object);
    reset();CHECK(reserve(0x10000000,0x10000000)==CELL_OK);
    CHECK(direct(35u*1024u*1024u)==CELL_OK);CHECK(disjoint(out(),35u*1024u*1024u));
    /* Alignment can move past the heap into a relocated injection window;
     * both exclusions must be revisited before committing a reservation. */
    reset();CHECK(reserve(0x18000000,0x10000000)==CELL_OK);
    CHECK(out()==0x80000000u);CHECK(disjoint(out(),0x20000000u));
    /* Ending exactly at the heap is legal; the next reservation skips it. */
    reset();ppu_hle_inject_base=0x20000000u;
    CHECK(reserve(0x10000000,0x100000)==CELL_OK);CHECK(out()==YZ_HEAP_BASE-0x10000000u);
    CHECK(reserve(0x100000,0x100000)==CELL_OK);CHECK(out()==VM_AUDIO_BASE+VM_AUDIO_SIZE);
    /* An actual sys_vm allocation and all manufactured audio-port spans
     * retain their contents after mapper ownership and memory writes. */
    reset();ppu_context c={0};c.gpr[3]=0x2000000;c.gpr[4]=0x10000;c.gpr[8]=0x10000;
    CHECK(sys_vm_memory_map(&c)==CELL_OK);uint32_t virtual_memory=out();
    CHECK(virtual_memory==SYS_VM_REGION_BASE);vm_write32(virtual_memory,0x11223344);
    CHECK(vm_commit(VM_AUDIO_BASE,VM_AUDIO_SIZE)==CELL_OK);
    for (uint32_t i=0;i<VM_AUDIO_SIZE/VM_AUDIO_PORT_SIZE;++i)
        vm_write32(VM_AUDIO_BASE+i*VM_AUDIO_PORT_SIZE,0xABCDEF00u+i);
    CHECK(reserve(0x20000000,0x10000000)==CELL_OK);uint32_t large=out();
    CHECK(disjoint(large,0x20000000));memset(vm_base+large,0,0x100000);
    CHECK(vm_read32(virtual_memory)==0x11223344);
    for (uint32_t i=0;i<VM_AUDIO_SIZE/VM_AUDIO_PORT_SIZE;++i)
        CHECK(vm_read32(VM_AUDIO_BASE+i*VM_AUDIO_PORT_SIZE)==0xABCDEF00u+i);
    /* Failure must neither publish an output nor consume the safe gap. */
    reset();vm_write32(0x10000,0xAABBCCDDu);
    CHECK(reserve(0x80000000,0x10000000)!=(int64_t)CELL_OK);CHECK(out()==0xAABBCCDDu);
    CHECK(reserve(0x100000,0x100000)==CELL_OK);CHECK(out()==0x40000000u);
    /* Heap bounds. A request larger than the window can never fit, and one
     * whose size + alignment + header wraps 32 bits used to come back as a tiny
     * block. */
    reset();
    CHECK(yz_heap_alloc(YZ_HEAP_END-YZ_HEAP_BASE+1u,16)==0);
    CHECK(yz_heap_alloc(0xFFFFFFF0u,16)==0);
    CHECK(yz_heap_alloc(0x100,0xFFFFFFF0u)==0);
    CHECK(s_heap_bump==0);
    /* A request that does not fit in what is left must fail WITHOUT moving the
     * bump pointer past the end of the window; the next small request still
     * succeeds, inside the window. (The pointer used to be advanced first, so
     * after one failure every later request failed or wrapped.) */
    reset();CHECK(vm_commit(YZ_HEAP_END-0x1000,0x1000)==CELL_OK);
    s_heap_bump=YZ_HEAP_END-0x1000;
    CHECK(yz_heap_alloc(0x10000,16)==0);
    CHECK(s_heap_bump==YZ_HEAP_END-0x1000);
    uint32_t tail=yz_heap_alloc(16,16);
    CHECK(tail>=YZ_HEAP_BASE && tail+16<=YZ_HEAP_END);
    CHECK(s_heap_bump<=YZ_HEAP_END);
    /* Deleting a heap made by _sys_heap_create_heap is CELL_OK. */
    u32 heap=_sys_heap_create_heap(0,0,0,0);
    CHECK(heap!=0);
    CHECK(_sys_heap_delete_heap(heap)==CELL_OK);
    vm_shutdown();printf("PASS %u mapper/HLE heap checks\n",checks);return 0;
}

/* Manufactured mapper/HLE-heap ownership regression; no title code or data.
 *
 * argv[1], optional, limits the ownership scenarios: "failclosed", "boundary",
 * "race" or "all" (default). The mapper scenarios above run either way. */
#include "../runtime/syscalls/sys_memory.h"
#include "../runtime/syscalls/sys_vm.h"
#include "../libs/system/sysPrxForUser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t ppu_hle_inject_base=0x70000000u;
static lv2_syscall_table table;
static unsigned checks;
/* Parking state for the ordered race: vm_write32 parks the ownership tag
 * write; the race scenario below drives the other side. */
static pthread_mutex_t s_park_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_park_cv = PTHREAD_COND_INITIALIZER;
static volatile int s_park_tag, s_parked, s_release_park;
/* The HLE heap uses the loader's big-endian guest accessors. */
static uint32_t vm_read32(uint32_t a) {
    uint8_t* p=vm_base+a;return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void vm_write32(uint32_t a,uint32_t v) {
    uint8_t* p=vm_base+a;p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;
    /* The ownership tag write (a nonzero value at a 16-byte-aligned address):
     * park a racing allocator there so an ordered delete can run against it. */
    if (__atomic_load_n(&s_park_tag, __ATOMIC_ACQUIRE) && v && (a & 15) == 0) {
        pthread_mutex_lock(&s_park_mtx);
        __atomic_store_n(&s_parked, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&s_release_park, __ATOMIC_ACQUIRE))
            pthread_cond_wait(&s_park_cv, &s_park_mtx);
        pthread_mutex_unlock(&s_park_mtx);
    }
}
extern int64_t sys_mmapper_allocate_memory(ppu_context*);
/* The heap code is concurrent by design (alloc and free take slot_lock in the
 * production file); the fixture gives it the real lock. */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static void slot_lock(void) { pthread_mutex_lock(&s_lock); }
static void slot_unlock(void) { pthread_mutex_unlock(&s_lock); }
/* Test-controlled realloc: fail one call, by index, so an ownership list that
 * cannot grow is deterministic. */
static int s_realloc_calls;
static int s_fail_next_realloc;
static void* test_realloc(void* p, size_t n)
{
    s_realloc_calls++;
    if (s_fail_next_realloc && s_realloc_calls == s_fail_next_realloc) {
        s_fail_next_realloc = 0;
        return NULL;
    }
    return realloc(p, n);
}
/* Exact production heap definitions/functions, copied without modification by
 * the driver (tools/test_mmapper_hle_heap.py: the heap table, create/destroy
 * and the allocator). */
#include "ps3emu/log_first.h"   /* the copied heap code logs through it */
#define realloc test_realloc
#include <heap-under-test.inc>
#undef realloc
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
static void wait_flag(const volatile int* f) {
    for (int i = 0; i < 5000 && !__atomic_load_n(f, __ATOMIC_ACQUIRE); i++) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
}
static volatile uintptr_t s_malloc_got;
static volatile int s_malloc_done, s_del_done;
static volatile s32 s_del_rc;
static void* race_malloc_main(void* p)
{
    uintptr_t heap = (uintptr_t)p;
    void* r = sys_heap_malloc((sys_heap_t)heap, 1024);
    __atomic_store_n(&s_malloc_got, (uintptr_t)r, __ATOMIC_RELEASE);
    __atomic_store_n(&s_malloc_done, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void* race_delete_main(void* p)
{
    uintptr_t heap = (uintptr_t)p;
    const s32 rc = _sys_heap_delete_heap((sys_heap_t)heap);
    __atomic_store_n(&s_del_rc, rc, __ATOMIC_RELEASE);
    __atomic_store_n(&s_del_done, 1, __ATOMIC_RELEASE);
    return NULL;
}
int main(int argc,char** argv) {
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
    /* Deleting a heap gives its blocks back: it used to answer CELL_OK and leave them in the window. The blocks of
     * a heap come back to the free lists, a block freed on its own is not freed twice, and another heap's block
     * stays allocated. */
    reset();
    u32 h1=_sys_heap_create_heap(0,0,0,0),h2=_sys_heap_create_heap(0,0,0,0);
    CHECK(h1!=0 && h2!=0 && h1!=h2);
    uint32_t a=(uint32_t)(uintptr_t)sys_heap_malloc(h1,1024);
    uint32_t b=(uint32_t)(uintptr_t)sys_heap_malloc(h1,1024);
    uint32_t other=(uint32_t)(uintptr_t)sys_heap_malloc(h2,1024);
    CHECK(a && b && other);
    CHECK(vm_read32(a-16)==h1 && vm_read32(b-16)==h1 && vm_read32(other-16)==h2);
    CHECK(sys_heap_free(h1,(void*)(uintptr_t)b)==CELL_OK);
    CHECK(_sys_heap_delete_heap(h1)==CELL_OK);
    uint32_t d=(uint32_t)(uintptr_t)sys_heap_malloc(h2,1024);
    uint32_t e=(uint32_t)(uintptr_t)sys_heap_malloc(h2,1024);
    CHECK((d==a || d==b) && (e==a || e==b) && d!=e);   /* the deleted heap's two blocks, each on a free list once */
    CHECK(other!=d && other!=e && vm_read32(other-12)==YZ_HEAP_MAGIC);
    CHECK(_sys_heap_delete_heap(h1)==(s32)CELL_ESRCH);   /* it is gone */
    /* A heap id this API never made leaves its blocks unowned: freed on their own, never by a delete. */
    uint32_t f=(uint32_t)(uintptr_t)sys_heap_malloc(0x7777,64);
    CHECK(f && vm_read32(f-16)==0);
    CHECK(_sys_heap_delete_heap(0x7777)==(s32)CELL_ESRCH && vm_read32(f-12)==YZ_HEAP_MAGIC);

    const char* mode = argc > 1 ? argv[1] : "all";

    /* A tracked heap whose ownership list cannot grow fails the allocation
     * closed: the block goes back to the free lists and NULL comes back. */
    if (!strcmp(mode, "all") || !strcmp(mode, "failclosed")) {
        reset();
        s_realloc_calls = 0;
        u32 hf = _sys_heap_create_heap(0,0,0,0);
        CHECK(hf != 0 && s_realloc_calls == 0);          /* tracking reused a free table entry */
        s_fail_next_realloc = s_realloc_calls + 1;       /* the first block's list growth */
        CHECK(sys_heap_malloc(hf, 1024) == NULL);
        CHECK(s_fail_next_realloc == 0);                 /* the injected failure fired */
        uint32_t ok = (uint32_t)(uintptr_t)sys_heap_malloc(hf, 1024);
        CHECK(ok && vm_read32(ok-16) == hf);
        CHECK(sys_heap_free(hf, (void*)(uintptr_t)ok) == CELL_OK);
        CHECK(_sys_heap_delete_heap(hf) == CELL_OK);
    }

    /* Both create paths draw from one id space: driving the out-param counter
     * up to a live _sys_heap_create_heap id cannot hand it to the other API,
     * and deleting one API's heap frees only its own blocks. */
    if (!strcmp(mode, "all") || !strcmp(mode, "boundary")) {
        reset();
        const uint32_t id_ea = YZ_HEAP_BASE + 0x80000u;    /* inside the committed heap window, above any block here */
        u32 tr = _sys_heap_create_heap(0,0,0,0);
        CHECK(tr != 0);
        for (int i = 0; i < 512 && s_next_heap_id < tr; i++) {
            CHECK(sys_heap_create_heap((sys_heap_t*)(uintptr_t)id_ea, 0, 0, 0, 0, 0) == CELL_OK);
            const u32 id = vm_read32(id_ea);
            CHECK(sys_heap_destroy_heap(id) == CELL_OK);
        }
        CHECK(sys_heap_create_heap((sys_heap_t*)(uintptr_t)id_ea, 0, 0, 0, 0, 0) == CELL_OK);
        const u32 tb = vm_read32(id_ea);
        CHECK(tb == tr + 1);                             /* ONE counter: the APIs hand out consecutive ids */
        uint32_t ta = (uint32_t)(uintptr_t)sys_heap_malloc(tb, 1024);
        uint32_t rb = (uint32_t)(uintptr_t)sys_heap_malloc(tr, 1024);
        CHECK(ta && rb && vm_read32(ta-16)==tb && vm_read32(rb-16)==tr);
        CHECK(sys_heap_destroy_heap(tb) == CELL_OK);
        CHECK(vm_read32(rb-16)==tr && vm_read32(rb-12)==YZ_HEAP_MAGIC);  /* the other API's block survives */
        uint32_t got = (uint32_t)(uintptr_t)sys_heap_malloc(tr, 1024);
        CHECK(got == ta && vm_read32(got-16)==tr);       /* the freed block came back exactly once */
        CHECK(_sys_heap_delete_heap(tr) == CELL_OK);
    }

    /* The allocate-and-own sequence is one critical section: a concurrent heap
     * delete cannot complete while the allocating call is parked between the
     * allocation and its ownership tag. */
    if (!strcmp(mode, "all") || !strcmp(mode, "race")) {
        reset();
        u32 hr = _sys_heap_create_heap(0,0,0,0);
        CHECK(hr != 0);
        __atomic_store_n(&s_malloc_done, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_del_done, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_parked, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_release_park, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_park_tag, 1, __ATOMIC_RELEASE);
        pthread_t ta, tb;
        CHECK(pthread_create(&ta, NULL, race_malloc_main, (void*)(uintptr_t)hr) == 0);
        wait_flag(&s_parked);
        CHECK(__atomic_load_n(&s_parked, __ATOMIC_ACQUIRE) == 1);  /* parked at the ownership tag */
        CHECK(pthread_create(&tb, NULL, race_delete_main, (void*)(uintptr_t)hr) == 0);
        for (int i = 0; i < 100; i++) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
        CHECK(__atomic_load_n(&s_del_done, __ATOMIC_ACQUIRE) == 0); /* the delete waits for the section */
        __atomic_store_n(&s_release_park, 1, __ATOMIC_RELEASE);
        pthread_mutex_lock(&s_park_mtx);
        pthread_cond_broadcast(&s_park_cv);
        pthread_mutex_unlock(&s_park_mtx);
        __atomic_store_n(&s_park_tag, 0, __ATOMIC_RELEASE);
        CHECK(pthread_join(ta, NULL) == 0);
        CHECK(pthread_join(tb, NULL) == 0);
        CHECK(__atomic_load_n(&s_malloc_got, __ATOMIC_ACQUIRE) != 0);
        CHECK(__atomic_load_n(&s_del_rc, __ATOMIC_ACQUIRE) == CELL_OK);
        uint32_t x = (uint32_t)(uintptr_t)sys_heap_malloc(0x999, 1024);
        uint32_t y = (uint32_t)(uintptr_t)sys_heap_malloc(0x999, 1024);
        CHECK(x && y && x != y && x == __atomic_load_n(&s_malloc_got, __ATOMIC_ACQUIRE));
        CHECK(vm_read32(x-12)==YZ_HEAP_MAGIC && vm_read32(y-12)==YZ_HEAP_MAGIC);
    }

    vm_shutdown();printf("PASS %u mapper/HLE heap checks\n",checks);return 0;
}

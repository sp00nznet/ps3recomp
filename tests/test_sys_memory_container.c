/* sys_memory containers take their memory out of the default pool, as LV2's do. Manufactured; no title code or data.
 *
 * Creating a container did not charge the default pool, so the free size a title read back for it did not drop;
 * allocations inside the container were also checked against the default pool, so a full default pool refused them.
 * A container with live allocations could be destroyed and its slot reused, and freeing the old allocation then
 * underflowed the new container's used_size. */
#include "../runtime/syscalls/sys_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t ppu_hle_inject_base=0x20000000u;
static lv2_syscall_table table;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
#define MB (1024u*1024u)
#define USER_TOTAL (213u*MB)
#define DEFAULT_CID 0xFFFFFFFFu
#define OUT 0x10000u
static uint32_t rd(uint32_t a) { uint8_t* p=vm_base+a; return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static int64_t create(uint32_t size) { ppu_context c={0};c.gpr[3]=OUT;c.gpr[4]=size;return sys_memory_container_create(&c); }
static int64_t destroy(uint32_t cid) { ppu_context c={0};c.gpr[3]=cid;return sys_memory_container_destroy(&c); }
static int64_t alloc(uint32_t size) { ppu_context c={0};c.gpr[3]=size;c.gpr[4]=0x400;c.gpr[5]=OUT;return sys_memory_allocate(&c); }
static int64_t alloc_in(uint32_t cid,uint32_t size) {
 ppu_context c={0};c.gpr[3]=size;c.gpr[4]=cid;c.gpr[5]=0x400;c.gpr[6]=OUT;return sys_memory_allocate_from_container(&c);
}
static int64_t release(uint32_t addr) { ppu_context c={0};c.gpr[3]=addr;return sys_memory_free(&c); }
/* {total, available} of a container, or of the default pool. */
static int64_t size_of(uint32_t cid,uint32_t* total,uint32_t* avail) {
 ppu_context c={0};c.gpr[3]=OUT+8;c.gpr[4]=cid;int64_t rc=sys_memory_container_get_size(&c);
 *total=rd(OUT+8);*avail=rd(OUT+12);return rc;
}
int main(void) {
 CHECK(vm_init()==CELL_OK);
 sys_memory_init(&table);
 uint32_t total,avail;

 /* Creating a container takes its size out of the default pool. */
 CHECK(size_of(DEFAULT_CID,&total,&avail)==CELL_OK); CHECK(total==USER_TOTAL&&avail==USER_TOTAL);
 CHECK(create(16*MB)==CELL_OK); uint32_t a=rd(OUT); CHECK(a!=0);
 CHECK(size_of(DEFAULT_CID,&total,&avail)==CELL_OK); CHECK(avail==USER_TOTAL-16*MB);
 { ppu_context c={0};c.gpr[3]=OUT+8;CHECK(sys_memory_get_user_memory_size(&c)==CELL_OK);CHECK(rd(OUT+12)==USER_TOTAL-16*MB); }
 CHECK(size_of(a,&total,&avail)==CELL_OK); CHECK(total==16*MB&&avail==16*MB);

 /* An allocation in the container is charged to it alone, not a second time to the default pool. */
 CHECK(alloc_in(a,4*MB)==CELL_OK); uint32_t in_a=rd(OUT);
 CHECK(size_of(a,&total,&avail)==CELL_OK); CHECK(avail==12*MB);
 CHECK(size_of(DEFAULT_CID,&total,&avail)==CELL_OK); CHECK(avail==USER_TOTAL-16*MB);

 /* With the default pool used up the container still has its own memory to hand out. */
 CHECK(alloc(USER_TOTAL-16*MB)==CELL_OK); uint32_t big=rd(OUT);
 CHECK(size_of(DEFAULT_CID,&total,&avail)==CELL_OK); CHECK(avail==0);
 CHECK(alloc_in(a,4*MB)==CELL_OK); uint32_t in_a2=rd(OUT);
 CHECK(alloc_in(a,8*MB+1)==(int64_t)(int32_t)CELL_ENOMEM);   /* but no more than its own */
 /* And a container the default pool cannot cover is refused. */
 CHECK(create(MB)==(int64_t)(int32_t)CELL_ENOMEM);
 CHECK(release(big)==CELL_OK);

 /* A container with live allocations cannot be destroyed, so its slot is not handed to another. */
 CHECK(destroy(a)==(int64_t)(int32_t)CELL_EBUSY);
 CHECK(size_of(a,&total,&avail)==CELL_OK); CHECK(total==16*MB&&avail==8*MB);
 CHECK(release(in_a)==CELL_OK); CHECK(release(in_a2)==CELL_OK);
 CHECK(size_of(a,&total,&avail)==CELL_OK); CHECK(avail==16*MB);

 /* Once empty it goes, and its memory goes back to the default pool. */
 CHECK(destroy(a)==CELL_OK);
 CHECK(size_of(a,&total,&avail)==(int64_t)(int32_t)CELL_ESRCH);
 CHECK(size_of(DEFAULT_CID,&total,&avail)==CELL_OK); CHECK(avail==USER_TOTAL);

 /* The slot's next container starts with nothing used. */
 CHECK(create(8*MB)==CELL_OK); uint32_t b=rd(OUT); CHECK(b==a);
 CHECK(size_of(b,&total,&avail)==CELL_OK); CHECK(total==8*MB&&avail==8*MB);
 CHECK(destroy(b)==CELL_OK);
 CHECK(size_of(DEFAULT_CID,&total,&avail)==CELL_OK); CHECK(avail==USER_TOTAL);

 vm_shutdown();printf("PASS %u memory container checks\n",checks);return 0;
}

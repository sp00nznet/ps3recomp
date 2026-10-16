/* Manufactured allocation-domain regression; no title code or data. */
#include "../runtime/syscalls/sys_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t ppu_hle_inject_base=0x20000000u;
static lv2_syscall_table table;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static uint32_t out(void) { uint8_t* p=vm_base+0x10000; return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static int64_t allocate(uint64_t size,uint64_t flags) {
 ppu_context c={0};c.gpr[3]=size;c.gpr[4]=flags;c.gpr[5]=0x10000;return sys_memory_allocate(&c);
}
static int64_t reserve(uint64_t size,uint64_t align) {
 ppu_context c={0};c.gpr[3]=size;c.gpr[5]=align;c.gpr[6]=0x10000;return sys_mmapper_allocate_address(&c);
}
static void reset(void) { sys_memory_init(&table); }
int main(void) {
 CHECK(vm_init()==CELL_OK);
 reset();
 CHECK(allocate(0x10000,0x200)==CELL_OK);uint32_t a=out();
 CHECK(a>=0x10000000u&&a<0x20000000u); CHECK((a&0xFFFFu)==0);
 CHECK(allocate(0x100000,0x400)==CELL_OK);uint32_t b=out();
 CHECK(b>=0x10000000u&&b<0x20000000u); CHECK((b&0xFFFFFu)==0);
 CHECK((a&0x40000000u)==0&&(b&0x40000000u)==0);
 CHECK(a+0x10000u<=b||b+0x100000u<=a);
 CHECK(reserve(0x100000,0x100000)==CELL_OK);uint32_t m=out();
 CHECK(m>=0x40000000u&&m<0xC0000000u); CHECK(m!=a&&m!=b);
 memset(vm_base+a,0x55,0x10000);ppu_context c={0};c.gpr[3]=a;
 CHECK(sys_memory_free(&c)==CELL_OK);
 CHECK(allocate(0x10000,0x200)==CELL_OK&&out()==a);
 CHECK(vm_base[a]==0&&vm_base[a+0xFFFF]==0);
 CHECK(sys_memory_free(&c)==CELL_OK); CHECK(sys_memory_free(&c)!=(int64_t)CELL_OK);
 reset();
 CHECK(allocate(UINT64_MAX,0x400)!=(int64_t)CELL_OK);
 CHECK(allocate(0x100000000ull,0x400)!=(int64_t)CELL_OK);
 CHECK(allocate(0xFFFFFFFFull,0x400)!=(int64_t)CELL_OK);
 CHECK(allocate(0,0x400)!=(int64_t)CELL_OK);
 CHECK(allocate(0x100000,0x400)==CELL_OK); CHECK(out()<0x20000000u);
 reset();
 CHECK(vm_commit(VM_PPU_TLS_BASE,0x100000u)==CELL_OK);
 vm_base[VM_PPU_TLS_BASE]=0xA5;
 CHECK(allocate(213u*1024u*1024u,0x200)==CELL_OK);
 CHECK(vm_base[VM_PPU_TLS_BASE]==0xA5);
 uint32_t before=out(); CHECK(allocate(0x10000,0x200)!=(int64_t)CELL_OK); CHECK(out()==before);
 reset();
 ppu_hle_inject_base=0x10000000u;
 CHECK(allocate(0x10000,0x200)==CELL_OK);CHECK(out()>=0x11000000u);
 ppu_hle_inject_base=0x20000000u;
 reset();
 CHECK(reserve(0x50000000ull,0x10000000)==CELL_OK); CHECK(out()==0x70000000u);
 CHECK(reserve(0x10000000ull,0x10000000)!=(int64_t)CELL_OK);
 CHECK(reserve(UINT64_MAX,0x10000000)!=(int64_t)CELL_OK);
 reset(); CHECK(reserve(0x100000,3)!=(int64_t)CELL_OK);
 reset(); CHECK(allocate(0x10000,0x200)==CELL_OK);CHECK(out()==a);
 vm_shutdown();printf("PASS %u user/mapper allocation checks\n",checks);return 0;
}

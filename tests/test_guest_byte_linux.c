#define _GNU_SOURCE
#include "../runtime/memory/guest_byte.h"
#include <sys/mman.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
static unsigned checks;
#define T(x) do { ++checks; if(!(x)){fprintf(stderr,"FAIL line%d\n",__LINE__);return 1;} }while(0)
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char**argv){
 const size_t page=(size_t)sysconf(_SC_PAGESIZE),size=0x10000+4*page;
 vm_base=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 T(vm_base!=MAP_FAILED);ppu_vm_size=(uint32_t)size;
 uint64_t ea=0x10000;uint8_t value=0x42;vm_base[ea]=0x21;
 T(ps3_guest_byte_copy(ea,&value,0)&&value==0x21);
 value=0x43;T(ps3_guest_byte_copy(ea,&value,1)&&vm_base[ea]==0x43);
 T(!ps3_guest_byte_copy(0xffff,&value,0));T(!ps3_guest_byte_copy(size,&value,0));
 T(!ps3_guest_byte_copy(UINT64_MAX,&value,0));T(!ps3_guest_byte_copy(ea,NULL,0));
 T(mprotect(vm_base+ea,page,PROT_READ)==0);value=0x91;
 T(!ps3_guest_byte_copy(ea,&value,1)&&vm_base[ea]==0x43);
 T(ps3_guest_byte_copy(ea,&value,0)&&value==0x43);
 T(mprotect(vm_base+ea,page,PROT_NONE)==0);value=0x91;
 T(!ps3_guest_byte_copy(ea,&value,0)&&value==0x91);
 T(!ps3_guest_byte_copy(ea,&value,1));
 T(mprotect(vm_base+ea,page,PROT_WRITE)==0);
 value=0x53;T(ps3_guest_byte_copy(ea,&value,1));T(!ps3_guest_byte_copy(ea,&value,0));
 T(mprotect(vm_base+ea,page,PROT_READ|PROT_WRITE)==0);
 T(ps3_guest_byte_copy(ea,&value,0)&&value==0x53);
 T(mprotect(vm_base+ea+page,page,PROT_NONE)==0);
 T(ps3_guest_byte_copy(ea+page-1,&value,0));T(!ps3_guest_byte_copy(ea+page,&value,0));
 T(munmap(vm_base+ea+2*page,page)==0);T(!ps3_guest_byte_copy(ea+2*page,&value,0));
 ppu_vm_size=0;T(!ps3_guest_byte_copy(0x100000000ULL,&value,0));
 T(!ps3_guest_byte_copy(ea+2*page,&value,1));ppu_vm_size=(uint32_t)size;
 if(argc>1&&!strcmp(argv[1],"benchmark")){
  double start=seconds();for(unsigned i=0;i<20000;++i){value=(uint8_t)i;T(ps3_guest_byte_copy(ea,&value,1));T(ps3_guest_byte_copy(ea,&value,0));}
  printf("BYTE_BENCH seconds=%.6f copies=40000\n",seconds()-start);
 }
 munmap(vm_base,size);printf("GUEST_BYTE_PASS checks=%u\n",checks);return 0;
}

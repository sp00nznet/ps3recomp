/* clang -std=c11 -O2 -D_GNU_SOURCE tests/test_guest_span_linux.c
 * Optional: ./a.out --bench [iterations] */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../runtime/memory/guest_span.h"

uint8_t* vm_base;
uint32_t ppu_vm_size;

static int failures;
#define CHECK(c) do{if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);failures++;}}while(0)

enum{PAGES=8};
static uint8_t*region;static size_t page;

static uint64_t ea_of(size_t index,size_t offset){return 0x10000ull+index*page+offset;}
static void use_region_base(void){vm_base=(uint8_t*)((uintptr_t)region-0x10000u);}
static void protect(size_t index,size_t count,int prot)
{
 if(mprotect(region+index*page,count*page,prot)!=0){perror("mprotect");exit(1);}
}
static int bytewise(uint64_t ea,size_t n)
{for(size_t i=0;i<n;i++)if(!ps3_guest_byte_writable(ea+i))return 0;return 1;}
/* Checks the span result and, for small spans, agreement with byte checks. */
static void expect(uint64_t ea,size_t n,int want,int line)
{
 int got=ps3_guest_span_writable(ea,n);
 if(got!=want){fprintf(stderr,"FAIL line %d: span(0x%llx,%zu)=%d want %d\n",line,(unsigned long long)ea,n,got,want);failures++;}
 if(n&&n<=64){int b=bytewise(ea,n);
  if(b!=want){fprintf(stderr,"FAIL line %d: bytewise(0x%llx,%zu)=%d want %d\n",line,(unsigned long long)ea,n,b,want);failures++;}}
}
#define EXPECT(ea,n,want) expect((ea),(n),(want),__LINE__)

static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static void bench(long iterations)
{
 volatile int sink=0;uint64_t ea=ea_of(1,page-2);double t0,t1,t2;
 t0=now();
 for(long i=0;i<iterations;i++)sink+=ps3_guest_byte_writable(ea)&&ps3_guest_byte_writable(ea+1)&&
  ps3_guest_byte_writable(ea+2)&&ps3_guest_byte_writable(ea+3);
 t1=now();
 for(long i=0;i<iterations;i++)sink+=ps3_guest_span_writable(ea,4);
 t2=now();
 CHECK(sink==2*iterations);
 printf("bench %ld iterations of 4-byte preflight: byte checks %.6f s (%.2f us/op), span check %.6f s (%.2f us/op)\n",
  iterations,t1-t0,(t1-t0)*1e6/iterations,t2-t1,(t2-t1)*1e6/iterations);
}

int main(int argc,char**argv)
{
 long sp=sysconf(_SC_PAGESIZE);if(sp<=0){fprintf(stderr,"page size unavailable\n");return 1;}page=(size_t)sp;
 void*m=mmap(NULL,PAGES*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(m==MAP_FAILED){perror("mmap");return 1;}region=m;
 if((uintptr_t)region<0x10000u){fprintf(stderr,"mapping below guest minimum offset\n");return 1;}
 for(size_t i=0;i<PAGES;i++)memset(region+i*page,0xA0+(int)i,page);
 /* 0,1 rw | 2 write-only | 3 read-only | 4 none | 5 gap | 6,7 rw */
 protect(2,1,PROT_WRITE);protect(3,1,PROT_READ);protect(4,1,PROT_NONE);
 if(munmap(region+5*page,page)!=0){perror("munmap");return 1;}
 ppu_vm_size=0;

 vm_base=NULL;CHECK(!ps3_guest_span_writable(0x10000,4));
 use_region_base();
 EXPECT(ea_of(0,0),4,1);
 EXPECT(ea_of(0,page-2),4,1);
 EXPECT(ea_of(1,page-2),4,1);              /* rw-p then -w-p */
 EXPECT(ea_of(2,8),4,1);                   /* writable without read */
 CHECK(ps3_guest_span_writable(ea_of(0,0),3*page));
 EXPECT(ea_of(2,page-2),4,0);              /* into read-only */
 EXPECT(ea_of(3,8),4,0);
 EXPECT(ea_of(4,8),4,0);                   /* PROT_NONE */
 EXPECT(ea_of(5,8),4,0);                   /* unmapped */
 EXPECT(ea_of(4,page-2),4,0);
 EXPECT(ea_of(5,page-2),4,0);              /* gap into rw */
 EXPECT(ea_of(6,0),4,1);
 CHECK(ps3_guest_span_writable(ea_of(6,0),2*page)); /* exact end of owned mapping */
 CHECK(!ps3_guest_span_writable(ea_of(0,0),PAGES*page));
 CHECK(!ps3_guest_span_writable(ea_of(0,0),0));
 CHECK(!ps3_guest_span_writable(0xFFFF,1));
 CHECK(!ps3_guest_span_writable(0xFFFE,4));
 CHECK(!ps3_guest_span_writable(0,4));
 CHECK(!ps3_guest_span_writable(UINT64_MAX,1));
 CHECK(!ps3_guest_span_writable(0x100000000ull,1));
 CHECK(!ps3_guest_span_writable(ea_of(0,0),SIZE_MAX));
 CHECK(!ps3_guest_span_writable(ea_of(0,0),(size_t)-2));

 /* Explicit guest limit inside the owned mapping. */
 ppu_vm_size=(uint32_t)ea_of(2,0);
 EXPECT(ea_of(1,page-4),4,1);
 EXPECT(ea_of(1,page-4),5,0);
 EXPECT(ea_of(2,0),1,0);
 ppu_vm_size=(uint32_t)ea_of(PAGES,0);
 EXPECT(ea_of(7,page-4),4,1);
 EXPECT(ea_of(7,page-3),4,0);
 ppu_vm_size=0;

 /* Default 4 GiB guest size: place the guest top at the mapping end. */
 if(UINTPTR_MAX>UINT32_MAX&&(uint64_t)(uintptr_t)region+PAGES*page>=0x100000000ull){
  vm_base=(uint8_t*)(uintptr_t)((uint64_t)(uintptr_t)region+PAGES*page-0x100000000ull);
  EXPECT(0xFFFFFFFCull,4,1);
  EXPECT(0xFFFFFFFFull,1,1);
  EXPECT(0xFFFFFFFDull,4,0);
  CHECK(!ps3_guest_span_writable(0x100000000ull,1));
  use_region_base();
 }else printf("skip default-size top test: mapping below 4 GiB\n");

 /* Host pointer bounds, checked arithmetically only. */
 {uintptr_t first=0,last=0;
  vm_base=(uint8_t*)(UINTPTR_MAX-0x10000u-2u);
  CHECK(ps3_guest_span_address(0x10000,2,&first,&last)&&last==UINTPTR_MAX-1);
  CHECK(!ps3_guest_span_address(0x10000,3,&first,&last));
  CHECK(!ps3_guest_span_address(0x10000,4,&first,&last));
  CHECK(!ps3_guest_span_address(0x10003,1,&first,&last));
  CHECK(!ps3_guest_span_writable(0x10000,4));
  use_region_base();
  CHECK(ps3_guest_span_address(ea_of(0,0),4,&first,&last)&&first==(uintptr_t)region&&last==(uintptr_t)region+3);}

 /* Protection changed between calls; no caching. */
 EXPECT(ea_of(6,16),4,1);
 protect(6,1,PROT_READ);EXPECT(ea_of(6,16),4,0);
 protect(6,1,PROT_READ|PROT_WRITE);EXPECT(ea_of(6,16),4,1);

 if(argc>1&&strcmp(argv[1],"--bench")==0){long n=argc>2?strtol(argv[2],NULL,10):1000;bench(n>0?n:1000);}

 /* Sentinels: preflight must not have written anything. */
 protect(0,5,PROT_READ);protect(6,2,PROT_READ);
 for(size_t i=0;i<PAGES;i++){if(i==5)continue;
  for(size_t j=0;j<page;j++)if(region[i*page+j]!=(uint8_t)(0xA0+i)){fprintf(stderr,"FAIL sentinel page %zu offset %zu\n",i,j);failures++;break;}}
 munmap(region,PAGES*page);
 if(failures){fprintf(stderr,"%d failure(s)\n",failures);return 1;}
 printf("guest_span tests passed\n");return 0;
}

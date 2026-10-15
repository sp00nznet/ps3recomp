/* Synthetic trusted host definitions only. Fresh process per mode. */
#include "spurs_native_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define T(x) do{if(!(x)){fprintf(stderr,"TARGET_REGISTRY_FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
static void entry(spu_context*c){(void)c;}
static void other(spu_context*c){(void)c;}
static uint8_t image[32],desc[64];
static const char sha[]="1111111111111111111111111111111111111111111111111111111111111111";
static spurs_native_target table[3];
#ifdef TARGET_ALLOC_FAULT
static unsigned fail_at,calls;
void* __real_malloc(size_t);
void* __wrap_malloc(size_t n){if(fail_at&&++calls==fail_at)return NULL;return __real_malloc(n);}
#endif
static spurs_native_definition definition(void)
{
 image[0]=1;desc[9]=2;desc[28]=1;
 table[0]=(spurs_native_target){0x8000,entry};table[1]=(spurs_native_target){0x8008,other};table[2]=(spurs_native_target){0x801C,other};
 spurs_native_definition d={0};d.image=image;d.image_size=32;d.descriptor_template=desc;d.descriptor_size=64;
 d.entry=entry;d.entry_pc=0x8000;d.layout=(spurs_native_layout){0x8000,0x6000,0x48F0,128,0x20000,0x1000,0x3FFF0};
 d.source_sha256=d.build_sha256=sha;d.targets=(spurs_native_targets){table,3};return d;
}
static void invalid(void)
{
 spurs_native_definition d=definition();const spurs_native_profile*out=(void*)(uintptr_t)1;
 for(unsigned bad=0;bad<12;bad++){
  d=definition();switch(bad){
  case 0:d.targets.entries=NULL;break;
  case 1:d.targets.count=0;break;
  case 2:d.targets.count=9;break;
  case 3:table[1].entry=NULL;break;
  case 4:table[1].pc=0x8009;break;
  case 5:table[1].pc=0x7FFC;break;
  case 6:table[2].pc=0x8020;break;
  case 7:table[2].pc=0x8008;break;
  case 8:table[1].pc=0x801C;table[2].pc=0x8008;break;
  case 9:table[0].entry=other;break;
  case 10:table[0].pc=0x8004;break;
  case 11:table[2].pc=0x3FFF0;break;
  }
  T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_INVALID&&out==(void*)(uintptr_t)1);
 }
 d=definition();T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_OK&&out);
 T(out->targets.count==3); /* Rejections did not publish a partial profile. */
}
static void copy_conflict(void)
{
 spurs_native_definition d=definition();const spurs_native_profile*p=NULL,*out=NULL;
 T(spurs_native_registry_register(&d,&p)==SPURS_NATIVE_OK);
 T(p->targets.entries!=table&&p->targets.count==3);
 T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_DUPLICATE&&out==p);
 table[1].entry=entry;out=(void*)(uintptr_t)1;
 T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_CONFLICT&&out==(void*)(uintptr_t)1);
 T(p->targets.entries[1].entry==other);table[1].entry=other;
 d.targets.count=2;T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_CONFLICT);
 d.targets=(spurs_native_targets){0};T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_CONFLICT);
 memset(table,0,sizeof table);T(p->targets.entries[2].pc==0x801C&&p->targets.entries[2].entry==other);
 T(spurs_native_registry_seal()==SPURS_NATIVE_OK);
 d=definition();T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_SEALED);
}
static void profile_identity(void)
{
 spurs_native_definition d=definition();const spurs_native_profile*a,*b,*out=NULL;
 T(spurs_native_registry_register(&d,&a)==SPURS_NATIVE_OK);
 image[0]=2;table[1].entry=entry;T(spurs_native_registry_register(&d,&b)==SPURS_NATIVE_OK);
 T(a!=b&&a->targets.entries[1].entry==other&&b->targets.entries[1].entry==entry);
 T(spurs_native_registry_seal()==SPURS_NATIVE_OK);
 desc[5]=4;spurs_job_snapshot s={0};s.descriptor=desc;s.image=image;s.image_size=32;s.descriptor_size=64;
 s.binary_ea=s.raw_binary_ea=0x40000;s.job_ea=0x30000;s.guest_limit=0x100000;
 uint64_t h=UINT64_C(1469598103934665603);for(unsigned i=0;i<32;i++){h^=image[i];h*=UINT64_C(1099511628211);}s.fingerprint=h;
 T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_OK&&out==b);
}
int main(int argc,char**argv)
{
 if(argc!=2)return 2;
#ifdef TARGET_ALLOC_FAULT
 if(!strcmp(argv[1],"allocation")){
  spurs_native_definition d=definition();const spurs_native_profile*out=(void*)(uintptr_t)1;
  for(unsigned i=1;i<=3;i++){calls=0;fail_at=i;T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_NO_MEMORY&&out==(void*)(uintptr_t)1);}
  fail_at=0;T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_OK&&out->targets.count==3);
  T(spurs_native_registry_seal()==SPURS_NATIVE_OK);puts("TARGET_REGISTRY_ALLOCATION_PASS");return 0;
 }
#endif
 if(!strcmp(argv[1],"invalid"))invalid();else if(!strcmp(argv[1],"copy-conflict"))copy_conflict();else if(!strcmp(argv[1],"identity"))profile_identity();else return 2;
 puts("TARGET_REGISTRY_PASS");return 0;
}

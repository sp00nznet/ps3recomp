/* Synthetic registry contracts. One fresh process per mode. No entry executes.
 * For allocation-failure mode compile ONLY registry.c with
 * -Dmalloc=ordered_test_malloc, and this test with -DORDERED_ALLOC_FAULT. */
#include "spurs_native_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define T(x) do { if(!(x)){fprintf(stderr,"ORDERED_REGISTRY_FAIL line=%d %s\n",__LINE__,#x);exit(1);} } while(0)
static uint8_t image[32],desc[64],bytes[2][16384];
static spurs_native_dma_step steps[17];
static unsigned executed;
static const char sha[]="0123456701234567012345670123456701234567012345670123456701234567";
static void entry(spu_context*c){(void)c;++executed;}
#ifdef ORDERED_ALLOC_FAULT
static unsigned fail_at,calls;
void *ordered_test_malloc(size_t n){if(fail_at&&++calls==fail_at)return NULL;return malloc(n);}
#endif
static void be16(uint8_t*p,unsigned x){p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static spurs_native_definition fixture(void)
{
 for(unsigned i=0;i<32;i++)image[i]=(uint8_t)(3*i+1);
 memset(desc,0,sizeof desc);be16(desc+8,2);be16(desc+28,64);be16(desc+30,1);
 for(unsigned i=0;i<16384;i++){bytes[0][i]=(uint8_t)(i*7+3);bytes[1][i]=(uint8_t)(i*11+9);}
 memset(steps,0,sizeof steps);
 steps[0]=(spurs_native_dma_step){0x40,0x20100,32,2,0x50000,bytes[0]};
 steps[1]=(spurs_native_dma_step){0x20,0x20500,32,3,0x51000,bytes[1]};
 spurs_native_definition d={0};d.image=image;d.descriptor_template=desc;d.image_size=32;d.descriptor_size=64;
 d.entry=entry;d.entry_pc=0x8000;d.layout=(spurs_native_layout){0x8000,0x6000,0x4800,128,0x20000,0x400,0x3FFF0};
 d.source_sha256=d.build_sha256=sha;d.input.kind=SPURS_NATIVE_INPUT_ORDERED_DMA;d.dma=(spurs_native_dma_sequence){steps,2};return d;
}
static const spurs_native_profile *add(spurs_native_definition*d)
{const spurs_native_profile*p=NULL;T(spurs_native_registry_register(d,&p)==SPURS_NATIVE_OK&&p);return p;}
static void invalid(spurs_native_definition*d)
{const spurs_native_profile*p=(const spurs_native_profile*)(uintptr_t)1;T(spurs_native_registry_register(d,&p)==SPURS_NATIVE_INVALID&&p==(const spurs_native_profile*)(uintptr_t)1);}
static void immutable(void)
{
 spurs_native_definition d=fixture();uint8_t im[32],de[64],expected[2][32];spurs_native_dma_step saved[2];
 memcpy(im,image,32);memcpy(de,desc,64);memcpy(saved,steps,sizeof saved);
 for(unsigned i=0;i<2;i++)memcpy(expected[i],bytes[i],32);
 const spurs_native_profile*p=add(&d);T(p->allocation_size==0x800&&p->layout.stack_size==0x400);
 T(p->dma.steps!=steps&&p->dma.count==2);
 memset(image,0xEE,32);memset(desc,0xDD,64);memset(steps,0,sizeof steps);memset(bytes,0xAB,sizeof bytes);
 T(!memcmp(p->image,im,32)&&!memcmp(p->descriptor_template+8,de+8,56));
 for(unsigned i=0;i<8;i++)T(p->descriptor_template[i]==0);
 for(unsigned i=0;i<2;i++){const spurs_native_dma_step*s=p->dma.steps+i;T(s->command==saved[i].command&&s->lsa==saved[i].lsa&&s->ea==saved[i].ea&&s->size==32&&s->tag==saved[i].tag);T(s->expected!=saved[i].expected&&!memcmp(s->expected,expected[i],32));}
}
static void duplicate(void)
{
 spurs_native_definition d=fixture();const spurs_native_profile*p=add(&d),*out=NULL;
 spurs_native_dma_step copy[2];uint8_t payload[2][32];memcpy(copy,steps,sizeof copy);
 for(unsigned i=0;i<2;i++){memcpy(payload[i],bytes[i],32);copy[i].expected=payload[i];}d.dma.steps=copy;
 T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_DUPLICATE&&out==p);
 for(unsigned n=0;n<6;n++){
  memcpy(copy,steps,sizeof copy);for(unsigned i=0;i<2;i++)copy[i].expected=payload[i];
  switch(n){case 0:payload[1][31]^=1;break;case 1:{spurs_native_dma_step t=copy[0];copy[0]=copy[1];copy[1]=t;break;}case 2:copy[0].tag++;break;case 3:copy[0].ea+=16;break;case 4:copy[0].lsa+=16;break;case 5:copy[0].command=0x20;break;}
  out=p;T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_CONFLICT&&out==p);if(!n)payload[1][31]^=1;
 }
 T(p->dma.steps[1].expected[31]==bytes[1][31]);
}
static void malformed(void)
{
 for(unsigned n=0;n<30;n++){
  spurs_native_definition d=fixture();
  switch(n){
  case 0:d.dma.count=0;break;case 1:d.dma.count=17;break;case 2:d.dma.steps=NULL;break;
  case 3:steps[0].expected=NULL;break;case 4:steps[0].command=0x41;break;
  case 5:steps[0].size=0;break;case 6:steps[0].size=3;break;case 7:steps[0].size=17;break;case 8:steps[0].size=16400;break;
  case 9:steps[0].tag=32;break;case 10:steps[0].ea=0xFFF0;break;case 11:steps[0].ea=UINT64_C(0x100000000);break;
  case 12:steps[0].ea=UINT64_C(0xFFFFFFF0);break;
  case 13:steps[0].lsa++;break;case 14:steps[0].ea++;break;case 15:steps[0].lsa++;steps[0].ea++;break;
  case 16:steps[0].lsa=0x1FFF0;break;case 17:steps[0].lsa=0x207F0;break;case 18:steps[0].lsa=UINT32_MAX-15;break;
  case 19:d.input.lsa=16;break;case 20:d.input.size=16;break;case 21:d.input.tag=1;break;case 22:d.input.ea=0x50000;break;case 23:d.input.expected[15]=1;break;
  case 24:d.input.kind=4;break;case 25:steps[0].size=8;steps[0].lsa+=4;steps[0].ea+=4;break;
  case 26:d.input.kind=SPURS_NATIVE_INPUT_NONE;break;
  case 27:d.layout.stack_size=UINT32_MAX-15;break;
  case 28:be16(desc+30,65535);break;case 29:be16(desc+28,63);break;
  }invalid(&d);
 }
 spurs_native_definition d=fixture();T(add(&d)); /* Failures did not publish identity. */
}
static void bounds(void)
{
 spurs_native_definition d=fixture();const spurs_native_profile*p=add(&d);T(p->allocation_size==0x800);
 d=fixture();d.layout.descriptor=0x20400;invalid(&d); /* Scratch, beyond requested stack. */
 d=fixture();d.layout.context=0x20780;invalid(&d);
 d=fixture();d.layout.return_pc=0x207FC;invalid(&d);
 d=fixture();d.layout.stack=0x3FC00;steps[0].lsa=0x3FD00;steps[1].lsa=0x3FD40;invalid(&d);
 d=fixture();be16(desc+30,0);d.layout.stack_size=0x410;be16(desc+28,65);desc[63]=1;steps[1].lsa=0x207E0;
 p=add(&d);T(p->allocation_size==0x800&&p->layout.stack_size==0x410);
 d=fixture();desc[63]=2;steps[1].ea=UINT64_C(0xFFFFFFE0);steps[1].lsa=0x207E0;T(add(&d));
}
static void sizes_budget(void)
{
 static const unsigned sizes[]={1,2,4,8,16,32,48,16384};
 for(unsigned k=0;k<8;k++){
  spurs_native_definition d=fixture();be16(desc+30,0x400);desc[63]=(uint8_t)k;d.dma.count=1;steps[0].size=sizes[k];
  const spurs_native_profile*p=add(&d);T(p->dma.steps[0].size==sizes[k]);
 }
 spurs_native_definition d=fixture();be16(desc+30,0x400);desc[63]=10;d.dma.count=16;
 for(unsigned i=0;i<16;i++)steps[i]=(spurs_native_dma_step){i&1?0x20:0x40,0x20100,16384,i,0x50000,bytes[i&1]};
 const spurs_native_profile*p=add(&d);T(p->dma.count==16);for(unsigned i=0;i<16;i++)T(!memcmp(p->dma.steps[i].expected,bytes[i&1],16384));
 /* 16*16384 is the exact aggregate cap; larger total requires a separately
  * invalid count or size, both covered by malformed. */
}
static void legacy(void)
{
 for(unsigned k=0;k<3;k++){
  spurs_native_definition d=fixture();desc[63]=(uint8_t)k;d.input.kind=k;d.dma=(spurs_native_dma_sequence){0};
  if(k){d.input.lsa=0x20100;d.input.size=4;d.input.tag=2;d.input.ea=0x50000;memcpy(d.input.expected,bytes[0],4);if(k==2)memcpy(d.input.expected+4,bytes[1],4);}
  T(add(&d));d.dma.steps=steps;invalid(&d);d.dma.steps=NULL;d.dma.count=1;invalid(&d);
 }
}
static void capacity_seal(void)
{
 spurs_native_definition d=fixture();const spurs_native_profile*first=NULL,*out=NULL;
 for(unsigned i=0;i<16;i++){desc[63]=(uint8_t)i;const spurs_native_profile*p=add(&d);if(!i)first=p;}
 desc[63]=16;out=first;T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_FULL&&out==first);
 desc[63]=0;T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_DUPLICATE&&out==first);
 T(spurs_native_registry_seal()==SPURS_NATIVE_OK);T(spurs_native_registry_seal()==SPURS_NATIVE_SEALED);
 T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_SEALED&&out==first);
}
static void allocation_failure(void)
{
#ifdef ORDERED_ALLOC_FAULT
 spurs_native_definition d=fixture();const spurs_native_profile*out=(const spurs_native_profile*)(uintptr_t)1;
 for(unsigned i=1;i<=4;i++){calls=0;fail_at=i;T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_NO_MEMORY&&out==(const spurs_native_profile*)(uintptr_t)1);}
 fail_at=0;T(add(&d));
#else
 fprintf(stderr,"allocation-failure requires documented isolated malloc instrumentation\n");exit(2);
#endif
}
int main(int argc,char**argv)
{
 if(argc!=2)return 2;
 if(!strcmp(argv[1],"immutable"))immutable();else if(!strcmp(argv[1],"duplicate"))duplicate();else if(!strcmp(argv[1],"malformed"))malformed();
 else if(!strcmp(argv[1],"bounds"))bounds();else if(!strcmp(argv[1],"sizes-budget"))sizes_budget();else if(!strcmp(argv[1],"legacy"))legacy();else if(!strcmp(argv[1],"capacity-seal"))capacity_seal();else if(!strcmp(argv[1],"allocation-failure"))allocation_failure();else return 2;
 T(!executed);puts("ORDERED_REGISTRY_PASS");return 0;
}

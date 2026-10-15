#include "spurs_native_registry.h"
#include "spu_context.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK registry_lock=SRWLOCK_INIT;
static void lock_registry(void){AcquireSRWLockExclusive(&registry_lock);}
static void unlock_registry(void){ReleaseSRWLockExclusive(&registry_lock);}
#else
#include <pthread.h>
static pthread_mutex_t registry_lock=PTHREAD_MUTEX_INITIALIZER;
static void lock_registry(void){pthread_mutex_lock(&registry_lock);}
static void unlock_registry(void){pthread_mutex_unlock(&registry_lock);}
#endif
static spurs_native_profile profiles[SPURS_NATIVE_REGISTRY_LIMIT];
static uint32_t profile_count;static int sealed;
static uint64_t fingerprint(const uint8_t*p,uint32_t size)
{uint64_t h=UINT64_C(1469598103934665603);for(uint32_t i=0;i<size;i++){h^=p[i];h*=UINT64_C(1099511628211);}return h;}
static uint32_t encoded_size(const uint8_t*d){return ((uint32_t)d[8]<<8|d[9])*16u;}
static uint64_t be64(const uint8_t*p){uint64_t n=0;for(unsigned i=0;i<8;i++)n=n<<8|p[i];return n;}
static int sizes(uint32_t image,uint32_t desc)
{return image&&image<=SPURS_JOB_SNAPSHOT_IMAGE_MAX&&!(image&15)&&desc>=SPURS_JOB_SNAPSHOT_DESC_MIN&&desc<=SPURS_JOB_SNAPSHOT_DESC_MAX&&!(desc&15);}
static int sha_valid(const char*p)
{if(!p)return 0;for(unsigned i=0;i<64;i++){char c=p[i];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return 0;}return p[64]==0;}
typedef struct span {uint32_t start,size;} span;
static int layout_valid(spurs_native_layout l,uint32_t size,uint32_t desc,uint32_t entry,uint32_t allocation)
{
 if((l.image|l.descriptor|l.context|l.stack|l.stack_size)&15||l.context_size!=128||l.stack_size<48||(l.return_pc&3)||(entry&3))return 0;
 span v[5]={{l.image,size},{l.descriptor,desc},{l.context,128},{l.stack,allocation},{l.return_pc,4}};
 for(unsigned i=0;i<5;i++){if(!v[i].size||v[i].start>=SPU_LS_SIZE||v[i].size>SPU_LS_SIZE-v[i].start)return 0;
  for(unsigned j=0;j<i;j++)if(v[i].start<v[j].start+v[j].size&&v[j].start<v[i].start+v[i].size)return 0;}
 return entry>=l.image&&entry-l.image<size;
}
static int layout_equal(spurs_native_layout a,spurs_native_layout b)
{return a.image==b.image&&a.descriptor==b.descriptor&&a.context==b.context&&a.context_size==b.context_size&&a.stack==b.stack&&a.stack_size==b.stack_size&&a.return_pc==b.return_pc;}
static int input_valid(spurs_native_input x,spurs_native_layout l,uint32_t allocation)
{
 if(x.kind==SPURS_NATIVE_INPUT_ORDERED_DMA){if(x.lsa||x.size||x.tag||x.ea)return 0;for(unsigned i=0;i<16;i++)if(x.expected[i])return 0;return 1;}
 if(x.kind==SPURS_NATIVE_INPUT_NONE){if(x.lsa||x.size||x.tag||x.ea)return 0;for(unsigned i=0;i<16;i++)if(x.expected[i])return 0;return 1;}
 if((x.kind!=SPURS_NATIVE_INPUT_EXACT_GET&&x.kind!=SPURS_NATIVE_INPUT_EXACT_GET_PAIR)||(x.size!=1&&x.size!=2&&x.size!=4&&x.size!=8&&x.size!=16)||x.tag>=32)return 0;
 if(x.kind==SPURS_NATIVE_INPUT_EXACT_GET_PAIR&&(x.size==16||!memcmp(x.expected,x.expected+x.size,x.size)))return 0;
 if(x.lsa<l.stack||x.lsa-l.stack>allocation||x.size>allocation-(x.lsa-l.stack)||x.ea<0x10000||x.ea>UINT32_MAX||x.size>UINT64_C(0x100000000)-x.ea)return 0;
 if((x.lsa&(x.size-1))||(x.ea&(x.size-1))||((x.lsa^x.ea)&15))return 0;
 unsigned used=x.kind==SPURS_NATIVE_INPUT_EXACT_GET_PAIR?2*x.size:x.size;
 for(unsigned i=used;i<16;i++)if(x.expected[i])return 0;return 1;
}
static int dma_valid(spurs_native_dma_sequence dma,spurs_native_input input,spurs_native_layout layout,uint32_t allocation)
{
 if(input.kind!=SPURS_NATIVE_INPUT_ORDERED_DMA)return !dma.count&&!dma.steps;
 if(!dma.steps||!dma.count||dma.count>SPURS_NATIVE_DMA_MAX_STEPS)return 0;
 uint32_t total=0;
 for(uint32_t i=0;i<dma.count;i++){
  const spurs_native_dma_step*s=&dma.steps[i];
  if((s->command!=MFC_GET_CMD&&s->command!=MFC_PUT_CMD)||!s->expected||s->tag>=32||
     !(s->size==1||s->size==2||s->size==4||s->size==8||(s->size>=16&&s->size<=16384&&!(s->size&15)))||
     s->lsa<layout.stack||s->lsa-layout.stack>allocation||s->size>allocation-(s->lsa-layout.stack)||
     s->ea<0x10000||s->ea>UINT32_MAX||s->size>UINT64_C(0x100000000)-s->ea||
     ((s->lsa^s->ea)&15)||
     (s->size>=16&&((s->lsa|s->ea)&15))||
     (s->size<16&&((s->lsa&(s->size-1))||(s->ea&(s->size-1))))||
     s->size>SPURS_NATIVE_DMA_MAX_BYTES-total)return 0;
  total+=s->size;
 }
 return 1;
}
static int dma_equal(spurs_native_dma_sequence a,spurs_native_dma_sequence b)
{
 if(a.count!=b.count)return 0;
 for(uint32_t i=0;i<a.count;i++){
  const spurs_native_dma_step*x=&a.steps[i],*y=&b.steps[i];
  if(x->command!=y->command||x->lsa!=y->lsa||x->size!=y->size||x->tag!=y->tag||x->ea!=y->ea||memcmp(x->expected,y->expected,x->size))return 0;
 }
 return 1;
}
static uint32_t allocation_size(const spurs_native_definition*d)
{
 /* BE16+0x1E contributes 16-byte units beyond the requested stack. */
 uint64_t extra=((uint32_t)d->descriptor_template[30]<<8|d->descriptor_template[31])*UINT64_C(16);
 uint64_t sum=(uint64_t)d->layout.stack_size+extra;
 uint64_t rounded=(sum+1023u)&~UINT64_C(1023);
 return rounded&&rounded<=SPU_LS_SIZE?(uint32_t)rounded:0;
}
static int input_equal(spurs_native_input a,spurs_native_input b)
{return a.kind==b.kind&&a.lsa==b.lsa&&a.size==b.size&&a.tag==b.tag&&a.ea==b.ea&&!memcmp(a.expected,b.expected,16);}
static int targets_valid(const spurs_native_definition*d)
{
 const spurs_native_targets*t=&d->targets;
 if(!t->count)return !t->entries;
 if(!t->entries||t->count>d->image_size/4)return 0;
 int has_entry=0;
 for(uint32_t i=0;i<t->count;i++){
  const spurs_native_target*x=&t->entries[i];
  if(!x->entry||(x->pc&3)||x->pc<d->layout.image||
     x->pc-d->layout.image>d->image_size-4||
     (i&&t->entries[i-1].pc>=x->pc))return 0;
  if(x->pc==d->entry_pc){if(x->entry!=d->entry)return 0;has_entry=1;}
 }
 return has_entry;
}
static int targets_equal(spurs_native_targets a,spurs_native_targets b)
{
 if(a.count!=b.count)return 0;
 for(uint32_t i=0;i<a.count;i++)if(a.entries[i].pc!=b.entries[i].pc||a.entries[i].entry!=b.entries[i].entry)return 0;
 return 1;
}
spurs_native_status spurs_native_registry_register(const spurs_native_definition*d,const spurs_native_profile**out)
{
 if(!d||!out||!d->image||!d->descriptor_template||!d->entry||!sizes(d->image_size,d->descriptor_size)||encoded_size(d->descriptor_template)!=d->image_size||(((uint32_t)d->descriptor_template[28]<<8|d->descriptor_template[29])*16u)!=d->layout.stack_size||!sha_valid(d->source_sha256)||!sha_valid(d->build_sha256))return SPURS_NATIVE_INVALID;
 uint32_t allocation=allocation_size(d);
 if(!allocation||!layout_valid(d->layout,d->image_size,d->descriptor_size,d->entry_pc,allocation)||!input_valid(d->input,d->layout,allocation)||!dma_valid(d->dma,d->input,d->layout,allocation)||!targets_valid(d))return SPURS_NATIVE_INVALID;
 lock_registry();if(sealed){unlock_registry();return SPURS_NATIVE_SEALED;}
 uint64_t fp=fingerprint(d->image,d->image_size);
 for(uint32_t i=0;i<profile_count;i++){spurs_native_profile*p=&profiles[i];
  if(p->fingerprint!=fp||p->image_size!=d->image_size||p->descriptor_size!=d->descriptor_size||memcmp(p->image,d->image,d->image_size)||memcmp(p->descriptor_template+8,d->descriptor_template+8,d->descriptor_size-8))continue;
  if(p->entry!=d->entry||p->entry_pc!=d->entry_pc||!layout_equal(p->layout,d->layout)||p->allocation_size!=allocation||!input_equal(p->input,d->input)||!dma_equal(p->dma,d->dma)||!targets_equal(p->targets,d->targets)||strcmp(p->source_sha256,d->source_sha256)||strcmp(p->build_sha256,d->build_sha256)){unlock_registry();return SPURS_NATIVE_CONFLICT;}
  *out=p;unlock_registry();return SPURS_NATIVE_DUPLICATE;
 }
 if(profile_count==SPURS_NATIVE_REGISTRY_LIMIT){unlock_registry();return SPURS_NATIVE_FULL;}
 uint8_t*image=(uint8_t*)malloc(d->image_size),*desc=(uint8_t*)malloc(d->descriptor_size);
 spurs_native_dma_step*steps=NULL;uint8_t*payload=NULL;
 spurs_native_target*targets=d->targets.count?(spurs_native_target*)malloc(d->targets.count*sizeof*targets):NULL;
 if(d->dma.count){
  uint32_t bytes=0;for(uint32_t i=0;i<d->dma.count;i++)bytes+=d->dma.steps[i].size;
  steps=(spurs_native_dma_step*)malloc(d->dma.count*sizeof*steps);
  payload=(uint8_t*)malloc(bytes);
 }
 if(!image||!desc||(d->dma.count&&(!steps||!payload))||(d->targets.count&&!targets)){free(image);free(desc);free(steps);free(payload);free(targets);unlock_registry();return SPURS_NATIVE_NO_MEMORY;}
 memcpy(image,d->image,d->image_size);memcpy(desc,d->descriptor_template,d->descriptor_size);memset(desc,0,8);
 if(steps){uint8_t*next=payload;for(uint32_t i=0;i<d->dma.count;i++){steps[i]=d->dma.steps[i];memcpy(next,d->dma.steps[i].expected,steps[i].size);steps[i].expected=next;next+=steps[i].size;}}
 if(targets)memcpy(targets,d->targets.entries,d->targets.count*sizeof*targets);
 spurs_native_profile*p=&profiles[profile_count];memset(p,0,sizeof*p);p->image=image;p->descriptor_template=desc;p->image_size=d->image_size;p->descriptor_size=d->descriptor_size;p->fingerprint=fp;p->entry=d->entry;p->entry_pc=d->entry_pc;p->layout=d->layout;p->input=d->input;p->dma.steps=steps;p->dma.count=d->dma.count;p->allocation_size=allocation;
 p->targets.entries=targets;p->targets.count=d->targets.count;
 memcpy(p->source_sha256,d->source_sha256,65);memcpy(p->build_sha256,d->build_sha256,65);++profile_count;*out=p;
 unlock_registry();return SPURS_NATIVE_OK;
}
spurs_native_status spurs_native_registry_seal(void)
{lock_registry();if(sealed){unlock_registry();return SPURS_NATIVE_SEALED;}sealed=1;unlock_registry();return SPURS_NATIVE_OK;}
static int snapshot_valid(const spurs_job_snapshot*s)
{
 if(!s||!s->image||!s->descriptor||!sizes(s->image_size,s->descriptor_size)||s->binary_flags||s->raw_binary_ea!=s->binary_ea||s->binary_ea<0x10000||(s->binary_ea&15)||s->job_ea<0x10000||(s->job_ea&15))return 0;
 if(!s->guest_limit||s->guest_limit>UINT64_C(0x100000000)||(uint64_t)s->binary_ea+s->image_size>s->guest_limit||(uint64_t)s->job_ea+s->descriptor_size>s->guest_limit)return 0;
 return be64(s->descriptor)==s->raw_binary_ea&&encoded_size(s->descriptor)==s->image_size&&fingerprint(s->image,s->image_size)==s->fingerprint;
}
spurs_native_status spurs_native_registry_resolve(const spurs_job_snapshot*s,const spurs_native_profile**out)
{
 if(!out)return SPURS_NATIVE_INVALID;
 lock_registry();if(!sealed){unlock_registry();return SPURS_NATIVE_NOT_SEALED;}
 if(!snapshot_valid(s)){unlock_registry();return SPURS_NATIVE_INVALID;}
 for(uint32_t i=0;i<profile_count;i++){const spurs_native_profile*p=&profiles[i];
  if(p->fingerprint==s->fingerprint&&p->image_size==s->image_size&&p->descriptor_size==s->descriptor_size&&!memcmp(p->image,s->image,s->image_size)&&!memcmp(p->descriptor_template+8,s->descriptor+8,s->descriptor_size-8)){*out=p;unlock_registry();return SPURS_NATIVE_OK;}}
 unlock_registry();return SPURS_NATIVE_NOT_FOUND;
}

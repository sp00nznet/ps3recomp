/* Synthetic host functions and initialized contexts. Real registry, exclusive
 * owner lease, native caller, channel dispatch and drain. No title or HLE/game
 * readiness claim. Fresh process per mode. No resolver/drain/HALT mocks. */
#include "spurs_native_job.h"
#include "spurs_native_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifndef TARGET_TEST_BASE
#define TARGET_TEST_BASE 0x8000u
#endif
#define PC(original) (TARGET_TEST_BASE+((original)-0x8000u))
#ifdef TARGET_DISPATCH_MT
extern void spu_indirect_branch_mt(spu_context*);
#define spu_indirect_branch spu_indirect_branch_mt
#endif
#define T(x) do{if(!(x)){fprintf(stderr,"INDIRECT_OWNER_FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
extern void spu_begin_image(int);
extern void spu_register_function(uint32_t,void (*)(spu_context*));
extern void spu_halt(spu_context*);
extern int spu_mfc_context_quiescent(spu_context*);
extern void spu_mfc_release(spu_context*);
static const char*mode;static unsigned entered,correct,wrong,continuations,recovering,second_hits;
static spu_context *first_context,*foreign_context;
static spu_context* parallel_context;static spurs_owner_token parallel_owner;static spurs_job_snapshot parallel_snapshot;
static spurs_owner_token first_owner;
static spurs_job_snapshot first_snapshot;
static const char sha[]="1111111111111111111111111111111111111111111111111111111111111111";
static int is(const char*s){return !strcmp(mode,s);}
static void global_wrong(spu_context*c){++wrong;c->pc=c->gpr[0]._u32[0];}
static void second(spu_context*c){++second_hits;SPU_RET(c);}
static void leaf(spu_context*c){++correct;SPU_RET(c);}
static void nested_target(spu_context*c)
{
 ++correct;uint32_t saved=c->gpr[0]._u32[0];c->gpr[0]._u32[0]=PC(0x8018);
 ++c->host_depth;c->pc=PC(0x8030);spu_indirect_branch(c);spu_drain_call(c,PC(0x8018));
 --c->host_depth;T(c->pc==PC(0x8018));c->gpr[0]._u32[0]=saved;SPU_RET(c);
}
static void cross_chunk(spu_context*c){++correct;c->gpr[0]._u32[0]=PC(0x8040);SPU_RET(c);}
static void continuation(spu_context*c){++continuations;c->gpr[0]._u32[0]=PC(0x8020);SPU_RET(c);}
static void jump(spu_context*c,uint32_t pc){c->pc=pc;g_spu_trampoline_fn=spu_indirect_branch;}
static void*legacy_thread(void*unused)
{
 (void)unused;spu_context*c=calloc(1,sizeof*c);T(c);spu_context_init(c,9);c->gpr[0]._u32[0]=0x3FFF0;
 c->pc=PC(0x8010);spu_indirect_branch(c);T(c->pc==0x3FFF0&&!g_spu_trampoline_fn);
 T(!spurs_native_job_resume(c));free(c);return NULL;
}
static void*parallel_thread(void*unused)
{
 (void)unused;spurs_native_job_result r=spurs_native_job_dispatch(&parallel_snapshot,parallel_owner,0);
 T(r.status==SPURS_NATIVE_JOB_RETURNED&&!spurs_native_job_resume(parallel_context));return NULL;
}
static void entry(spu_context*c)
{
 ++entered;T(spurs_native_job_resume(c));
 if(recovering||c==parallel_context){jump(c,PC(0x8010));return;}
 if(is("nested")){
  spurs_native_job_result busy=spurs_native_job_dispatch(&first_snapshot,first_owner,0);T(busy.status==SPURS_NATIVE_JOB_BUSY);
 }
 if(is("tls")){pthread_t t;T(!pthread_create(&t,NULL,legacy_thread,NULL));T(!pthread_join(t,NULL));}
 if(is("parallel")){pthread_t t;T(!pthread_create(&t,NULL,parallel_thread,NULL));T(!pthread_join(t,NULL));}
 if(is("foreign")){foreign_context->pc=PC(0x8010);spu_indirect_branch(foreign_context);T(0);}
 if(is("halt")){c->status=SPU_STATUS_STOPPED_BY_HALT;spu_halt(c);T(0);}
 if(is("stop")){c->status=SPU_STATUS_STOPPED_BY_STOP;c->stop_code=0x123;return;}
 if(is("legacy-state")){c->image_id=99;c->resident_ovl=99;c->resident_task=99;c->resident_code[0].image_id=99;c->resident_code[0].lsa=PC(0x8000);c->resident_code[0].size=0x100;}
 if(is("zero")){c->image_id=99;c->steps=1;jump(c,0);return;}
 if(is("sentinel")){jump(c,c->gpr[0]._u32[0]);return;}
 if(is("bad-sentinel")){c->pc=c->gpr[0]._u32[0];c->host_depth=2;spu_indirect_branch(c);T(0);}
 if(is("misaligned")){jump(c,PC(0x8011));return;}
 if(is("range")){jump(c,(PC(0x8010)+SPU_LS_SIZE));return;}
 if(is("unknown")||is("data")||is("compat-unknown")){jump(c,is("data")?PC(0x80F0):PC(0x8800));return;}
 if(is("no-table")){jump(c,PC(0x8010));return;}
 if(is("return-miss")||is("return-alias")||is("nested")||is("cross-chunk")||is("cross-chunk-private")){
  uint32_t saved=c->gpr[0]._u32[0];c->gpr[0]._u32[0]=PC(0x8020);++c->host_depth;
  c->pc=PC(0x8010);spu_indirect_branch(c); /* actual indirect callee */
  if(is("return-miss"))c->pc=PC(0x80F0);
  if(is("return-alias"))c->pc=(PC(0x8020)+SPU_LS_SIZE);
  spu_drain_call(c,PC(0x8020));--c->host_depth;
  T(c->pc==PC(0x8020));c->gpr[0]._u32[0]=saved;
  c->pc=saved;return;
 }
 jump(c,PC(0x8010));
}
static spurs_owner_token owner(uint32_t ea,spu_context**out)
{
 spu_context*c=calloc(1,sizeof*c);T(c);spu_context_init(c,ea>>16);c->spurs_owner_ea=ea;
 c->host_depth=7;c->drain_ret_pc=0x1234;c->steps=9;
 spurs_owner_token o;T(spurs_owner_reserve(ea,1,&o)==SPURS_OWNER_OK);
 T(spurs_owner_bind_worker(o,0,c)==SPURS_OWNER_OK&&spurs_owner_publish(o)==SPURS_OWNER_OK);
 *out=c;return o;
}
static void close_owner(spurs_owner_token o,spu_context*c)
{
 T(spurs_owner_close_begin(o)==SPURS_OWNER_OK);void*contexts[1];uint32_t count=0;
 T(spurs_owner_close_finish(o,contexts,1,&count)==SPURS_OWNER_OK&&count==1&&contexts[0]==c);
 spu_mfc_release(c);free(c);
}
static spurs_job_snapshot snapshot(uint8_t*image,uint8_t*desc)
{
 spurs_job_snapshot s={0};s.image=image;s.descriptor=desc;s.image_size=256;s.descriptor_size=64;
 s.binary_ea=s.raw_binary_ea=0x40000;s.job_ea=0x30000;s.guest_limit=0x100000;
 uint64_t h=UINT64_C(1469598103934665603);for(unsigned i=0;i<256;i++){h^=image[i];h*=UINT64_C(1099511628211);}s.fingerprint=h;return s;
}
static void clean(spu_context*c)
{
 T(!g_spu_trampoline_fn&&c->host_depth==7&&c->drain_ret_pc==0x1234&&c->steps==9);
 T(!c->irq_frame&&spu_mfc_context_quiescent(c)&&!spurs_native_job_resume(c));
}
int main(int argc,char**argv)
{
 if(argc!=2)return 2;mode=argv[1];
 static const char*cases[]={"correct","wildcard","wrong-image","unknown","data","misaligned","range","foreign","sentinel","bad-sentinel","nested","cross-chunk","cross-chunk-private","return-miss","return-alias","halt","stop","tls","parallel","no-table","legacy-state","zero","compat-unknown"};
 unsigned found=0;for(unsigned i=0;i<sizeof cases/sizeof*cases;i++)found|=is(cases[i]);if(!found)return 2;
 if(is("compat-unknown")){
#ifdef _WIN32
 _putenv_s("PS3_STRICT_BOOT","0");
#else
 setenv("PS3_STRICT_BOOT","0",1);
#endif
 }
 uint8_t image[256]={1},image2[256]={2},desc[64]={0};desc[5]=4;desc[8]=0;desc[9]=16;desc[28]=1;
 first_snapshot=snapshot(image,desc);spurs_job_snapshot snap2=snapshot(image2,desc);
 spurs_native_target table[]={{PC(0x8000),entry},{PC(0x8010),(is("cross-chunk")||is("cross-chunk-private"))?cross_chunk:is("nested")?nested_target:leaf},{PC(0x8030),leaf},{PC(0x8040),continuation}};
 spurs_native_definition d={0};d.image=image;d.descriptor_template=desc;d.image_size=256;d.descriptor_size=64;d.entry=entry;d.entry_pc=PC(0x8000);
 d.layout=(spurs_native_layout){PC(0x8000),0x6000,0x48F0,128,0x20000,0x1000,0x3FFF0};d.source_sha256=d.build_sha256=sha;
 if(!is("no-table"))d.targets=(spurs_native_targets){table,4};const spurs_native_profile*p=NULL;
 T(spurs_native_registry_register(&d,&p)==SPURS_NATIVE_OK);
 spurs_native_target table2[]={{PC(0x8000),entry},{PC(0x8010),second}};d.image=image2;d.targets=(spurs_native_targets){table2,2};
 T(spurs_native_registry_register(&d,&p)==SPURS_NATIVE_OK&&spurs_native_registry_seal()==SPURS_NATIVE_OK);
 memset(table,0,sizeof table);memset(table2,0,sizeof table2); /* copy ownership */
 spu_begin_image(is("wrong-image")?99:0);spu_register_function(PC(0x8010),global_wrong);
 spu_register_function(PC(0x80F0),global_wrong);spu_register_function(PC(0x8800),global_wrong);
 spu_register_function(0x3FFF0,global_wrong); /* Sentinel never executes code. */
 if(!is("cross-chunk-private"))spu_register_function(PC(0x8040),global_wrong);
 first_owner=owner(0x10000,&first_context);foreign_context=calloc(1,sizeof*foreign_context);T(foreign_context);spu_context_init(foreign_context,8);
 if(is("parallel")){parallel_owner=owner(0x18000,&parallel_context);parallel_snapshot=snap2;}
 spurs_native_job_result r=spurs_native_job_dispatch(&first_snapshot,first_owner,0);
 spurs_native_indirect_failure why=SPURS_NATIVE_INDIRECT_OK;
 if(is("misaligned"))why=SPURS_NATIVE_INDIRECT_MISALIGNED;
 if(is("range")||is("return-alias"))why=SPURS_NATIVE_INDIRECT_RANGE;
 if(is("foreign"))why=SPURS_NATIVE_INDIRECT_CONTEXT;
 if(is("unknown")||is("data")||is("return-miss")||is("no-table")||is("zero")||is("compat-unknown"))why=SPURS_NATIVE_INDIRECT_UNKNOWN;
 if(is("bad-sentinel"))why=SPURS_NATIVE_INDIRECT_RETURN;
 spurs_native_job_status want=why?SPURS_NATIVE_JOB_INDIRECT_REJECTED:is("halt")?SPURS_NATIVE_JOB_HALTED:is("stop")?SPURS_NATIVE_JOB_STOPPED:SPURS_NATIVE_JOB_RETURNED;
 T(r.status==want&&r.indirect_failure==why&&entered==1+(unsigned)is("parallel"));
 T(wrong==(unsigned)is("tls"));
 if(why)T(r.halt_landing&&r.spu_status==SPU_STATUS_STOPPED_BY_HALT&&(r.indirect_pc||is("zero")));
 if(why){
  uint32_t pc=is("misaligned")?PC(0x8011):is("range")?PC(0x8010)+SPU_LS_SIZE:
   is("return-alias")?PC(0x8020)+SPU_LS_SIZE:is("foreign")||is("no-table")?PC(0x8010):
   is("zero")?0:is("bad-sentinel")?0x3FFF0:is("data")||is("return-miss")?PC(0x80F0):PC(0x8800);
  T(r.indirect_pc==pc);
 }
 if(is("foreign"))T(foreign_context->status==SPU_STATUS_STOPPED);
 if(want==SPURS_NATIVE_JOB_RETURNED)T(r.pc==0x3FFF0&&r.spu_status==SPU_STATUS_RUNNING&&!r.halt_landing);
 if(is("cross-chunk")||is("cross-chunk-private"))T(continuations==1&&correct==1);
 if(is("nested"))T(correct==2);
 clean(first_context);spurs_worker_lease lease;T(spurs_owner_acquire_worker(first_owner,0,&lease)==SPURS_OWNER_OK&&lease.context==first_context);T(spurs_owner_release_worker(lease)==SPURS_OWNER_OK);
 if(is("parallel")){T(second_hits==1);clean(parallel_context);close_owner(parallel_owner,parallel_context);}
 unsigned prior_status=first_context->status;recovering=1;spu_context*c2;spurs_owner_token o2=owner(0x18000,&c2);
 spurs_native_job_result rr=spurs_native_job_dispatch(&snap2,o2,0);
 T(rr.status==SPURS_NATIVE_JOB_RETURNED&&second_hits==1+(unsigned)is("parallel")&&first_context->status==prior_status);clean(c2);
 close_owner(o2,c2);close_owner(first_owner,first_context);free(foreign_context);
 printf("INDIRECT_OWNER_PASS mode=%s status=%u category=%u\n",mode,(unsigned)r.status,(unsigned)why);return 0;
}

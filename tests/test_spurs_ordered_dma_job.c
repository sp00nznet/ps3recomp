/* Synthetic entry, actual SPURS owner/lease/caller and production MFC channels.
 * No DMA mocks, direct observer calls, fabricated events or title bytes.
 * One fresh process per mode. Logical rejection modes assert and exit zero. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "spurs_native_job.h"
#include "spurs_native_registry.h"
#include "cellSpurs.h"
#include "sys_event.h"
#include "lv2_syscall_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
extern uint8_t*vm_base;
extern uint32_t ppu_vm_size;
extern void spu_wrch(spu_context*,uint32_t,u128);
extern u128 spu_rdch(spu_context*,uint32_t);
extern int spu_mfc_context_quiescent(spu_context*);
extern int (*g_spu_user_event_hook)(spu_context*,uint32_t);
#define C(x) do{if(!(x)){fprintf(stderr,"ORDERED_JOB_FAIL line=%d %s\n",__LINE__,#x);exit(1);}}while(0)
static const char*mode;
static unsigned entered,returned_gets,returned_puts,recovering,emit_event;
static uint8_t image[16],desc[128],a[32],b[32],output[32];
static const char sha[]="1111111111111111111111111111111111111111111111111111111111111111";
static int is(const char*s){return !strcmp(mode,s);}
static int fault(const char*s){return !recovering&&is(s);}
static void wr(spu_context*c,uint32_t ch,uint32_t n){u128 v={{0}};v._u32[0]=n;spu_wrch(c,ch,v);}
static void transfer(spu_context*c,unsigned index)
{
 uint32_t command=index==1?0x20:0x40,lsa=index==2?0x20500:0x20100;
 uint32_t ea=index==0?0x50000:index==1?0x51000:0x52000,size=32,tag=2;
 if(is("pair")){size=8;ea=0x50000;}
 if(index==0){
  if(fault("guest-unsupported"))command=0x44;
  if(fault("guest-overlay"))lsa=0x8000;
  if(fault("wrong-command"))command=0x20;
  if(fault("wrong-order")){command=0x20;ea=0x51000;}
  if(fault("wrong-ea"))ea+=16;if(fault("wrong-ls"))lsa+=16;
  if(fault("wrong-size"))size=16;if(fault("wrong-tag"))tag=3;
 }
 wr(c,MFC_LSA,lsa);wr(c,MFC_EAH,0);wr(c,MFC_EAL,ea);wr(c,MFC_Size,size);wr(c,MFC_TagID,tag);wr(c,MFC_Cmd,command);
 if(command==0x40)++returned_gets;else ++returned_puts;
 wr(c,MFC_WrTagMask,4);wr(c,MFC_WrTagUpdate,2);u128 tags=spu_rdch(c,MFC_RdTagStat);C(tags._u32[0]&4);
}
static void entry(spu_context*c)
{
 ++entered;C(c->spurs_owner_ea==(recovering?0x18000u:0x10000u));
 C(c->gpr[0]._u32[0]==0x3FFF0&&c->gpr[1]._u32[0]==0x207D0);
 C(c->gpr[3]._u32[0]==0x48F0&&c->gpr[4]._u32[0]==0x6000&&c->gpr[126]._u32[0]==0x8000);
 C(spu_ls_read32(c,0x4960)==0x20000&&spu_ls_read32(c,0x207D0)==0x207F0);
 for(unsigned i=4;i<48;i++)C(c->ls[0x207D0+i]==0);
 /* Caller stages the acquired snapshot even after registration sources change. */
 for(unsigned i=0;i<16;i++)C(c->ls[0x8000+i]==(uint8_t)(i+1));
 C(c->ls[0x601D]==0x40&&c->ls[0x601F]==1);
 if(!is("none")&&!fault("missing-all")){
  transfer(c,0);
  C(!memcmp(c->ls+0x20100,a,is("pair")?8:32));
  if(!is("pair")){
   /* Output is computed from real delivered bytes, never copied from oracle. */
   for(unsigned i=0;i<32;i++)c->ls[0x20100+i]^=0x5A;
   if(fault("wrong-put-payload"))c->ls[0x2011F]^=1;
   transfer(c,1);
   if(!fault("missing-last")){
    if(fault("late-get-mismatch"))vm_base[0x5201F]^=1;
    transfer(c,2);C(!memcmp(c->ls+0x20500,b,32));
    if(fault("extra"))transfer(c,2);
   }
  }
 }
 if(emit_event){wr(c,SPU_WrOutMbox,0);wr(c,SPU_WrOutIntrMbox,0x50000000);}
 if(fault("pending"))wr(c,SPU_WrOutMbox,0x1234);
 c->pc=c->gpr[0]._u32[0];
}
static void be32(uint8_t*p,uint32_t n){p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
static uint32_t read32(uint32_t ea){uint8_t*p=vm_base+ea;return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint64_t read64(uint32_t ea){return (uint64_t)read32(ea)<<32|read32(ea+4);}
static void receive(uint32_t q,unsigned count,uint32_t worker)
{
 ppu_context p={0};p.gpr[3]=q;p.gpr[4]=0x24000;p.gpr[5]=1;p.gpr[6]=0x25000;
 C(sys_event_queue_tryreceive(&p)==0&&read32(0x25000)==count);
 if(count)C(read64(0x24000)==UINT64_C(0xFFFFFFFF53505501)&&read64(0x24008)==worker&&read64(0x24010)==UINT64_C(0x1000000000)&&read64(0x24018)==0);
}
static spurs_owner_token initialize(uint32_t ea)
{spurs_owner_token o={0};C(cellSpursInitialize((CellSpurs*)(uintptr_t)ea,1,100,100,1)==0);C(spurs_owner_retain(ea,&o)==SPURS_OWNER_OK);return o;}
static void finish(uint32_t ea,spurs_owner_token o)
{
 C(spurs_owner_release(o)==SPURS_OWNER_OK);int32_t rc;unsigned tries=0;
 do{rc=cellSpursFinalize((CellSpurs*)(uintptr_t)ea);if(rc==CELL_SPURS_CORE_ERROR_BUSY){C(++tries<5000);
#ifdef _WIN32
 Sleep(1);
#else
 usleep(1000);
#endif
 }}while(rc==CELL_SPURS_CORE_ERROR_BUSY);C(rc==0);
}
static int parse(void)
{
 static const char*all[]={"guest","guest-unsupported","guest-overlay","guest-strict-off","ordered","wrong-command","wrong-order","wrong-ea","wrong-ls","wrong-size","wrong-tag","wrong-put-payload","late-get-mismatch","missing-all","missing-last","extra","strict-off","none","pair","pending","busy","stale","nonquiescent"};
 for(unsigned i=0;i<sizeof all/sizeof*all;i++)if(is(all[i]))return 1;return 0;
}
int main(int argc,char**argv)
{
 if(argc!=2)return 2;mode=argv[1];if(!parse())return 2;
#ifdef _WIN32
 _putenv_s("PS3_STRICT_BOOT",is("strict-off")||is("guest-strict-off")?"0":"1");
#else
 setenv("PS3_STRICT_BOOT",is("strict-off")||is("guest-strict-off")?"0":"1",1);
#endif
 C(vm_base==NULL);vm_base=(uint8_t*)calloc(1,0x100000);C(vm_base);ppu_vm_size=0x100000;
 for(unsigned i=0;i<16;i++)image[i]=(uint8_t)(i+1);
 for(unsigned i=0;i<32;i++){a[i]=(uint8_t)(i*7+3);b[i]=(uint8_t)(i*13+9);output[i]=a[i]^0x5A;}
 be32(desc+4,0x40000);desc[9]=1;desc[0x1D]=0x40;desc[0x1F]=1;
 memcpy(vm_base+0x30000,desc,128);memcpy(vm_base+0x40000,image,16);
 memset(vm_base+0x4FFF0,0xC7,0x2030);memcpy(vm_base+0x50000,a,32);memcpy(vm_base+0x52000,b,32);
 uint8_t before[0x2030];memcpy(before,vm_base+0x4FFF0,sizeof before);
 spurs_job_snapshot snap={0};C(spurs_job_snapshot_acquire(vm_base,0x100000,0x30000,128,&snap)==SPURS_JOB_SNAPSHOT_OK);
 spurs_native_dma_step sequence[3]={{0x40,0x20100,32,2,0x50000,a},{0x20,0x20100,32,2,0x51000,output},{0x40,0x20500,32,2,0x52000,b}};
 spurs_native_definition d={0};d.image=image;d.descriptor_template=desc;d.image_size=16;d.descriptor_size=128;d.entry=entry;d.entry_pc=0x8000;
 d.layout=(spurs_native_layout){0x8000,0x6000,0x48F0,128,0x20000,0x400,0x3FFF0};d.source_sha256=d.build_sha256=sha;
 if(is("pair")){d.input.kind=SPURS_NATIVE_INPUT_EXACT_GET_PAIR;d.input.lsa=0x20100;d.input.size=8;d.input.tag=2;d.input.ea=0x50000;memcpy(d.input.expected,a,8);memcpy(d.input.expected+8,b,8);}
 else if(!strncmp(mode,"guest",5)){d.input.kind=SPURS_NATIVE_INPUT_GUEST_DMA;}
 else if(!is("none")){d.input.kind=SPURS_NATIVE_INPUT_ORDERED_DMA;d.dma=(spurs_native_dma_sequence){sequence,3};}
 const spurs_native_profile*p=NULL;C(spurs_native_registry_register(&d,&p)==SPURS_NATIVE_OK&&p&&p->allocation_size==0x800);C(spurs_native_registry_seal()==SPURS_NATIVE_OK);
 /* Invalidate source structures, not payload oracle used by assertions. */
 memset(sequence,0,sizeof sequence);memset(image,0xEE,sizeof image);memset(desc,0xDD,sizeof desc);
 memset(vm_base+0x30000,0xCC,128);memset(vm_base+0x40000,0xBB,16);
 lv2_syscall_table table;lv2_syscall_table_init(&table);lv2_register_all_syscalls(&table);C(g_spu_user_event_hook);
 spurs_owner_token owner=initialize(0x10000);spurs_worker_lease lease={0};C(spurs_owner_acquire_worker(owner,0,&lease)==SPURS_OWNER_OK);
 spu_context*c=(spu_context*)lease.context;C(c&&c->ls==c->ls_store);uint32_t worker=c->spu_id;
 memset(c->ls,0xA5,SPU_LS_SIZE);c->host_depth=7;c->drain_ret_pc=0x1234;c->steps=9;C(spu_mfc_context_quiescent(c));
 if(is("nonquiescent"))spu_channel_write(&c->ch_in_mbox,0xDEADBEEF);
 if(!is("busy"))C(spurs_owner_release_worker(lease)==SPURS_OWNER_OK);
 emit_event=is("ordered")||is("none")||is("pair")||is("guest");uint32_t queue=0;
 if(emit_event){queue=sys_event_queue_create_direct(0,1);C(queue);C(cellSpursAttachLv2EventQueue((CellSpurs*)(uintptr_t)0x10000,queue,(uint8_t*)(uintptr_t)0x23000,1)==0);C(vm_base[0x23000]==16);receive(queue,0,worker);}
 spurs_owner_token requested=owner;if(is("stale"))requested.generation++;
 spurs_native_job_result r=spurs_native_job_dispatch(&snap,requested,0);
 C(r.profile_registered); /* Identity survives later admission/execution failure. */
 int shape=(!strncmp(mode,"wrong-",6)&&!is("wrong-put-payload"))||is("guest-unsupported")||is("guest-overlay");
 int admission=is("strict-off")||is("guest-strict-off")||is("busy")||is("stale")||is("nonquiescent");
 int negative=shape||is("wrong-put-payload")||is("late-get-mismatch")||is("missing-all")||is("missing-last")||is("extra");
 spurs_native_job_status want=admission?(is("busy")?SPURS_NATIVE_JOB_BUSY:is("stale")?SPURS_NATIVE_JOB_INVALID:SPURS_NATIVE_JOB_UNQUALIFIED):negative?SPURS_NATIVE_JOB_INPUT_MISMATCH:is("pending")?SPURS_NATIVE_JOB_PENDING:SPURS_NATIVE_JOB_RETURNED;
 C(r.status==want&&entered==(unsigned)!admission);
 if(is("busy"))C(spurs_owner_release_worker(lease)==SPURS_OWNER_OK);
 C(spurs_owner_acquire_worker(owner,0,&lease)==SPURS_OWNER_OK&&lease.context==c);
 C(c->host_depth==7&&c->drain_ret_pc==0x1234&&c->steps==9&&!g_spu_trampoline_fn);
 unsigned gets=admission||shape||is("missing-all")||is("none")?0:is("wrong-put-payload")||is("late-get-mismatch")||is("missing-last")||is("pair")?1:2;
 unsigned puts=admission||shape||is("missing-all")||is("none")||is("pair")||is("wrong-put-payload")?0:1;
 C(r.completed_gets==gets&&r.completed_puts==puts&&r.completed_transfers==gets+puts&&returned_gets==gets&&returned_puts==puts);
 if(want==SPURS_NATIVE_JOB_RETURNED)C(r.pc==0x3FFF0&&r.spu_status==SPU_STATUS_RUNNING&&!r.halt_landing);
 if(negative&&!is("missing-all")&&!is("missing-last"))C(r.halt_landing&&r.spu_status==SPU_STATUS_STOPPED_BY_HALT&&c->status==SPU_STATUS_STOPPED_BY_HALT);
 if(is("missing-all")||is("missing-last"))C(!r.halt_landing&&r.pc==0x3FFF0);
 if(puts)memcpy(before+0x1010,output,32);
 if(is("late-get-mismatch")){before[0x202F]^=1;C(!memcmp(c->ls+0x20500,vm_base+0x52000,32));}
 C(!memcmp(before,vm_base+0x4FFF0,sizeof before)); /* Includes all DMA guest canaries. */
 for(unsigned i=0;i<SPU_LS_SIZE;i++){
  int staged=!admission&&((i>=0x8000&&i<0x8010)||(i>=0x6000&&i<0x6080)||(i>=0x4960&&i<0x4964)||(i>=0x207D0&&i<0x20800));
  int first=gets&&i>=0x20100&&i<0x20100+(is("pair")?8u:32u);
  int last=(gets==2||is("late-get-mismatch"))&&i>=0x20500&&i<0x20520;
  if(!staged&&!first&&!last)C(c->ls[i]==0xA5);
 }
 if(emit_event){receive(queue,1,worker);receive(queue,0,worker);C(!c->ch_in_mbox.count&&!c->ch_out_mbox.count);}
 if(is("pending"))C(spu_channel_read(&c->ch_out_mbox)==0x1234);
 if(is("nonquiescent"))C(spu_channel_read(&c->ch_in_mbox)==0xDEADBEEF);
 C(spu_mfc_context_quiescent(c));C(spurs_owner_release_worker(lease)==SPURS_OWNER_OK);
 if(negative){
  /* Preserve failed owner's HALT status while proving TLS cleanup on a
   * distinct real owner. No reset/manufactured readiness on failed context. */
  unsigned old_status=c->status;recovering=1;emit_event=0;memcpy(vm_base+0x52000,b,32);
  spurs_owner_token second=initialize(0x18000);spurs_native_job_result rr=spurs_native_job_dispatch(&snap,second,0);
  C(rr.status==SPURS_NATIVE_JOB_RETURNED&&rr.completed_gets==2&&rr.completed_puts==1&&rr.completed_transfers==3);
  C(c->status==old_status);C(spurs_owner_acquire_worker(second,0,&lease)==SPURS_OWNER_OK&&lease.context!=c);
  C(spu_mfc_context_quiescent((spu_context*)lease.context));C(spurs_owner_release_worker(lease)==SPURS_OWNER_OK);finish(0x18000,second);
 }
 if(queue){C(cellSpursDetachLv2EventQueue((CellSpurs*)(uintptr_t)0x10000,16)==0);ppu_context pp={0};pp.gpr[3]=queue;C(sys_event_queue_destroy(&pp)==0);}
 finish(0x10000,owner);spurs_job_snapshot_release(&snap);free(vm_base);vm_base=NULL;
 printf("ORDERED_JOB_PASS mode=%s status=%u gets=%u puts=%u transfers=%u\n",mode,(unsigned)r.status,r.completed_gets,r.completed_puts,r.completed_transfers);return 0;
}

#include "spurs_native_job.h"
#include "spurs_native_registry.h"
#include "ps3emu/strict_boot.h"
#include <string.h>
extern int spu_run_with_halt(void (*)(spu_context*),spu_context*);
extern void spu_halt(spu_context*);
extern int spu_mfc_context_quiescent(spu_context*);
typedef struct native_frame {
 spu_context* context;
 const spurs_native_profile* profile;
 unsigned completed_gets;
 int input_mismatch;
} native_frame;
static SPU_CONTEXT_TLS native_frame* active_native;
static void input_reject(native_frame* frame)
{
 frame->input_mismatch=1;
 frame->context->status=SPU_STATUS_STOPPED_BY_HALT;
 spu_halt(frame->context);
}
void spurs_native_job_mfc_before(spu_context* ctx,uint32_t command)
{
 native_frame* f=active_native;
 if(!f||f->context!=ctx)return;
 const spurs_native_input* in=&f->profile->input;
 if(in->kind!=SPURS_NATIVE_INPUT_EXACT_GET||f->completed_gets||
    command!=MFC_GET_CMD||ctx->mfc_lsa!=in->lsa||
    (((uint64_t)ctx->mfc_eah<<32)|ctx->mfc_eal)!=in->ea||
    ctx->mfc_size!=in->size||ctx->mfc_tag!=in->tag)input_reject(f);
}
void spurs_native_job_mfc_after(spu_context* ctx,uint32_t command)
{
 native_frame* f=active_native;
 if(!f||f->context!=ctx)return;
 const spurs_native_input* in=&f->profile->input;
 if(in->kind!=SPURS_NATIVE_INPUT_EXACT_GET||command!=MFC_GET_CMD||
    memcmp(ctx->ls+in->lsa,in->expected,in->size))input_reject(f);
 ++f->completed_gets;
}
static void native_landing(spu_context* ctx)
{
 native_frame* f=active_native;
 ctx->host_depth=1;
 f->profile->entry(ctx);
 if(ctx->status==SPU_STATUS_RUNNING&&g_spu_trampoline_fn)
  spu_drain_call(ctx,f->profile->layout.return_pc);
}
spurs_native_job_result spurs_native_job_dispatch(
 const spurs_job_snapshot* snapshot,spurs_owner_token owner,uint32_t worker_index)
{
 spurs_native_job_result result={SPURS_NATIVE_JOB_INVALID,0,0,0,0,0};
 const spurs_native_profile* profile=NULL;
 spurs_native_status resolved=spurs_native_registry_resolve(snapshot,&profile);
 if(resolved!=SPURS_NATIVE_OK){
  result.status=resolved==SPURS_NATIVE_NOT_SEALED?SPURS_NATIVE_JOB_REGISTRY_OPEN:
   resolved==SPURS_NATIVE_NOT_FOUND?SPURS_NATIVE_JOB_UNREGISTERED:SPURS_NATIVE_JOB_INVALID;
  return result;
 }
 /* EXACT_GET relies on the strict MFC's checked synchronous copy and fatal
  * transfer errors. Compatibility DMA semantics cannot certify delivered data. */
 if(profile->input.kind==SPURS_NATIVE_INPUT_EXACT_GET&&!ps3_strict_boot_enabled()){
  result.status=SPURS_NATIVE_JOB_UNQUALIFIED;return result;
 }
 if(active_native||g_spu_trampoline_fn){result.status=SPURS_NATIVE_JOB_BUSY;return result;}
 spurs_worker_lease lease={0};
 spurs_owner_status acquired=spurs_owner_acquire_worker(owner,worker_index,&lease);
 if(acquired!=SPURS_OWNER_OK){result.status=acquired==SPURS_OWNER_BUSY?
  SPURS_NATIVE_JOB_BUSY:SPURS_NATIVE_JOB_INVALID;return result;}
 spu_context* ctx=(spu_context*)lease.context;
 if(!ctx||ctx->ls!=ctx->ls_store||ctx->spurs_owner_ea!=owner.ea||
    ctx->spurs_pm_loaded||ctx->policy_mode||ctx->irq_frame||ctx->irq_saved||
    ctx->int_enable||ctx->event_mask||ctx->event_status||
    (ctx->status!=SPU_STATUS_STOPPED&&ctx->status!=SPU_STATUS_RUNNING)||
    !spu_mfc_context_quiescent(ctx)){
  result.status=SPURS_NATIVE_JOB_UNQUALIFIED;goto release;
 }
 const spurs_native_layout* layout=&profile->layout;
 /* Validation/resolution and all admission checks precede these mutations.
  * Complete owned copies, including the dynamic descriptor binaryEA, stage
  * together. No registry canonical template is substituted for the snapshot. */
 memcpy(ctx->ls+layout->image,snapshot->image,snapshot->image_size);
 memcpy(ctx->ls+layout->descriptor,snapshot->descriptor,snapshot->descriptor_size);
 spu_ls_write32(ctx,layout->context+0x70,layout->stack);
 uint32_t initial_sp=layout->stack+layout->stack_size-48;
 memset(ctx->ls+initial_sp,0,48);
 spu_ls_write32(ctx,initial_sp,layout->stack+layout->stack_size-16);
 ctx->gpr[0]._u32[0]=layout->return_pc;
 ctx->gpr[1]._u32[0]=initial_sp;
 ctx->gpr[3]._u32[0]=layout->context;
 ctx->gpr[4]._u32[0]=layout->descriptor;
 ctx->gpr[126]._u32[0]=layout->image;
 uint32_t old_depth=ctx->host_depth,old_drain=ctx->drain_ret_pc,old_steps=ctx->steps;
 ctx->pc=profile->entry_pc;ctx->status=SPU_STATUS_RUNNING;
 native_frame frame={ctx,profile,0,0};active_native=&frame;
 result.halt_landing=spu_run_with_halt(native_landing,ctx);
 result.pc=ctx->pc;result.spu_status=ctx->status;result.stop_code=ctx->stop_code;
 result.completed_gets=frame.completed_gets;
 if(frame.input_mismatch)result.status=SPURS_NATIVE_JOB_INPUT_MISMATCH;
 else if(result.halt_landing||ctx->status==SPU_STATUS_STOPPED_BY_HALT)result.status=SPURS_NATIVE_JOB_HALTED;
 else if(ctx->status!=SPU_STATUS_RUNNING)result.status=SPURS_NATIVE_JOB_STOPPED;
 else if(ctx->pc!=layout->return_pc||g_spu_trampoline_fn||ctx->host_depth!=1)result.status=SPURS_NATIVE_JOB_BAD_RETURN;
 else if(profile->input.kind==SPURS_NATIVE_INPUT_EXACT_GET&&frame.completed_gets!=1)result.status=SPURS_NATIVE_JOB_INPUT_MISMATCH;
 else if(ctx->event_mask||!spu_mfc_context_quiescent(ctx))result.status=SPURS_NATIVE_JOB_PENDING;
 else result.status=SPURS_NATIVE_JOB_RETURNED;
 active_native=NULL;g_spu_trampoline_fn=NULL;
 ctx->host_depth=old_depth;ctx->drain_ret_pc=old_drain;ctx->steps=old_steps;
release:
 if(spurs_owner_release_worker(lease)!=SPURS_OWNER_OK)result.status=SPURS_NATIVE_JOB_INVALID;
 return result;
}

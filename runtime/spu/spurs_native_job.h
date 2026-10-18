#ifndef PS3RECOMP_SPURS_NATIVE_JOB_H
#define PS3RECOMP_SPURS_NATIVE_JOB_H
#include "spurs_job_snapshot.h"
#include "spurs_owner_lifetime.h"
#include "spu_context.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum spurs_native_job_status {
 SPURS_NATIVE_JOB_RETURNED=0,SPURS_NATIVE_JOB_UNREGISTERED,
 SPURS_NATIVE_JOB_REGISTRY_OPEN,SPURS_NATIVE_JOB_INVALID,
 SPURS_NATIVE_JOB_BUSY,SPURS_NATIVE_JOB_UNQUALIFIED,
 SPURS_NATIVE_JOB_HALTED,SPURS_NATIVE_JOB_STOPPED,
 SPURS_NATIVE_JOB_BAD_RETURN,SPURS_NATIVE_JOB_INPUT_MISMATCH,
 SPURS_NATIVE_JOB_PENDING,SPURS_NATIVE_JOB_INDIRECT_REJECTED
} spurs_native_job_status;
typedef enum spurs_native_indirect_failure {
 SPURS_NATIVE_INDIRECT_OK=0,SPURS_NATIVE_INDIRECT_CONTEXT,
 SPURS_NATIVE_INDIRECT_MISALIGNED,SPURS_NATIVE_INDIRECT_RANGE,
 SPURS_NATIVE_INDIRECT_UNKNOWN,SPURS_NATIVE_INDIRECT_RETURN
} spurs_native_indirect_failure;
typedef struct spurs_native_job_result {
 spurs_native_job_status status;
 uint32_t pc,spu_status,stop_code,completed_gets;
 int halt_landing;
 uint32_t completed_puts,completed_transfers;
 spurs_native_indirect_failure indirect_failure;
 uint32_t indirect_pc;
 /* Identity/layout resolution succeeded, even if later admission/execution
  * failed. Diagnostic capture filtering only; this does not qualify a job. */
 int profile_registered;
} spurs_native_job_result;
/* No guest descriptor/image rereads, arbitrary dispatch entry or readiness
 * assertion. Resolution requires a sealed trusted-startup registry. The owner
 * must remain retained by the chain; an exclusive actual worker lease is held
 * throughout staging, native execution and post-return inspection.
 * This implemented ABI preserves spare context/register lanes, writes only
 * context+0x70, and supplies the registered raw-entry caller frame. A title
 * must independently establish that this ABI and its narrow input policy fit.
 * An ordinary C return is insufficient: pending work, STOP, HALT, wrong PC,
 * transfer mismatch and incomplete required DMA have separate outcomes.
 * Completed counters count synchronous transfers that passed the registered
 * contract. A rejected GET may already have changed LS. Earlier successful
 * PUTs/GETs remain real effects if a later ordered step fails. */
spurs_native_job_result spurs_native_job_dispatch(
 const spurs_job_snapshot*,spurs_owner_token,uint32_t worker_index);
/* Production channel integration only, around the real synchronous command.
 * Outside a native call these are inert. Within it, unexpected commands halt
 * before transfer; GET bytes are inspected after real DMA and before generated
 * code resumes. PUT bytes are inspected in LS before the real copy. These
 * hooks never read guest input or synthesize it. */
void spurs_native_job_mfc_before(spu_context*,uint32_t command);
void spurs_native_job_mfc_after(spu_context*,uint32_t command);
/* Production dispatch/drain only. TLS-bound actual leased context and exact
 * immutable profile, independent of mutable legacy image/resident IDs.
 * Indirect returns zero only outside a native frame. Inside it returns one
 * with the exact host target (NULL for normal sentinel return), or halts.
 * resume is a nonmutating active-frame query for the drain's continuation path.
 * It also rejects a foreign context, never authorizing its legacy fallback. */
int spurs_native_job_indirect(spu_context*,void (**target)(spu_context*));
int spurs_native_job_resume(spu_context*);
#ifdef __cplusplus
}
#endif
#endif

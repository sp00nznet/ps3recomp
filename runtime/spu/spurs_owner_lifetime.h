#ifndef PS3RECOMP_SPURS_OWNER_LIFETIME_H
#define PS3RECOMP_SPURS_OWNER_LIFETIME_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SPURS_OWNER_LIMIT 4u
#define SPURS_OWNER_WORKER_LIMIT 6u
typedef enum spurs_owner_status { SPURS_OWNER_OK=0, SPURS_OWNER_INVALID,
 SPURS_OWNER_STALE, SPURS_OWNER_BUSY, SPURS_OWNER_FULL, SPURS_OWNER_STATE } spurs_owner_status;
typedef struct spurs_owner_token {uint32_t ea,slot;uint64_t generation;} spurs_owner_token;
typedef struct spurs_worker_lease {spurs_owner_token owner;uint32_t index;uint64_t cookie;void *context;} spurs_worker_lease;
/* Host bookkeeping only. No contexts/threads/events are created or freed.
 * A token does not prove guest initialization, MFC quiescence or worker identity.
 * All output pointers are valid caller-owned host memory; failures leave outputs
 * and registry unchanged. NULL contexts and zero tokens/leases are invalid. */
spurs_owner_status spurs_owner_reserve(uint64_t ea,uint32_t nspus,spurs_owner_token *out);
spurs_owner_status spurs_owner_bind_worker(spurs_owner_token owner,uint32_t index,void *context);
spurs_owner_status spurs_owner_publish(spurs_owner_token owner);
/* Abort forgets INITIALIZING bindings; caller still owns/frees allocations. */
spurs_owner_status spurs_owner_abort(spurs_owner_token owner);
spurs_owner_status spurs_owner_retain(uint64_t ea,spurs_owner_token *out);
/* One successful retain requires one release; copied owner tokens do not
 * manufacture additional retained handles. Underflow is rejected. */
spurs_owner_status spurs_owner_release(spurs_owner_token owner);
spurs_owner_status spurs_owner_acquire_worker(spurs_owner_token owner,uint32_t index,spurs_worker_lease *out);
spurs_owner_status spurs_owner_validate_worker(spurs_worker_lease lease);
spurs_owner_status spurs_owner_release_worker(spurs_worker_lease lease);
spurs_owner_status spurs_owner_close_begin(spurs_owner_token owner);
/* Restore ACTIVE after exclusive inspection finds pending work; no data reset. */
spurs_owner_status spurs_owner_close_cancel(spurs_owner_token owner);
/* CLOSING only, all-or-nothing. Contexts remain caller-owned; caller must
 * qualify actual worker join/MFC lifetime before freeing returned pointers. */
spurs_owner_status spurs_owner_close_finish(spurs_owner_token owner,void **contexts,uint32_t capacity,uint32_t *count);
#ifdef __cplusplus
}
#endif
#endif

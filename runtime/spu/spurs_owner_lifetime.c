#include "spurs_owner_lifetime.h"
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK owner_mutex=SRWLOCK_INIT;
static void owner_lock(void){AcquireSRWLockExclusive(&owner_mutex);}
static void owner_unlock(void){ReleaseSRWLockExclusive(&owner_mutex);}
#else
#include <pthread.h>
static pthread_mutex_t owner_mutex=PTHREAD_MUTEX_INITIALIZER;
static void owner_lock(void){pthread_mutex_lock(&owner_mutex);}
static void owner_unlock(void){pthread_mutex_unlock(&owner_mutex);}
#endif
enum owner_state {OWNER_EMPTY,OWNER_INITIALIZING,OWNER_ACTIVE,OWNER_CLOSING};
typedef struct owner_record {
 spurs_owner_token token;uint32_t nspus;enum owner_state state;uint64_t refs;
 void *contexts[SPURS_OWNER_WORKER_LIMIT];uint64_t cookies[SPURS_OWNER_WORKER_LIMIT];
} owner_record;
static owner_record records[SPURS_OWNER_LIMIT];
/* Refuse exhaustion before increment: neither counter can ever wrap/reuse. */
static uint64_t next_generation=1,next_cookie=1;
static int valid_ea(uint64_t ea){return ea>=0x10000&&ea<=UINT32_MAX&&!(ea&127);}
static int valid_token(spurs_owner_token t){return valid_ea(t.ea)&&t.slot<SPURS_OWNER_LIMIT&&t.generation;}
static int exact_token(spurs_owner_token t)
{owner_record*r=&records[t.slot];return r->state!=OWNER_EMPTY&&r->token.ea==t.ea&&r->token.generation==t.generation;}
static int has_leases(const owner_record*r)
{for(unsigned j=0;j<r->nspus;j++)if(r->cookies[j])return 1;return 0;}
spurs_owner_status spurs_owner_reserve(uint64_t ea,uint32_t nspus,spurs_owner_token*out)
{
 if(!out||!valid_ea(ea)||!nspus||nspus>SPURS_OWNER_WORKER_LIMIT)return SPURS_OWNER_INVALID;
 owner_lock();unsigned free_slot=SPURS_OWNER_LIMIT;
 for(unsigned i=0;i<SPURS_OWNER_LIMIT;i++){
  if(records[i].state!=OWNER_EMPTY&&records[i].token.ea==ea){owner_unlock();return SPURS_OWNER_BUSY;}
  if(records[i].state==OWNER_EMPTY&&free_slot==SPURS_OWNER_LIMIT)free_slot=i;
 }
 if(free_slot==SPURS_OWNER_LIMIT||next_generation==UINT64_MAX){owner_unlock();return SPURS_OWNER_FULL;}
 owner_record*r=&records[free_slot];memset(r,0,sizeof*r);
 r->token.ea=(uint32_t)ea;r->token.slot=free_slot;r->token.generation=next_generation++;
 r->nspus=nspus;r->state=OWNER_INITIALIZING;*out=r->token;
 owner_unlock();return SPURS_OWNER_OK;
}
spurs_owner_status spurs_owner_bind_worker(spurs_owner_token t,uint32_t index,void*context)
{
 if(!valid_token(t)||!context||index>=SPURS_OWNER_WORKER_LIMIT)return SPURS_OWNER_INVALID;
 owner_lock();if(!exact_token(t)){owner_unlock();return SPURS_OWNER_STALE;}
 owner_record*r=&records[t.slot];
 if(r->state!=OWNER_INITIALIZING){owner_unlock();return SPURS_OWNER_STATE;}
 if(index>=r->nspus){owner_unlock();return SPURS_OWNER_INVALID;}
 if(r->contexts[index]){owner_unlock();return SPURS_OWNER_BUSY;}
 for(unsigned i=0;i<SPURS_OWNER_LIMIT;i++)if(records[i].state!=OWNER_EMPTY)
  for(unsigned j=0;j<records[i].nspus;j++)if(records[i].contexts[j]==context){owner_unlock();return SPURS_OWNER_BUSY;}
 r->contexts[index]=context;owner_unlock();return SPURS_OWNER_OK;
}
spurs_owner_status spurs_owner_publish(spurs_owner_token t)
{
 if(!valid_token(t))return SPURS_OWNER_INVALID;
 owner_lock();if(!exact_token(t)){owner_unlock();return SPURS_OWNER_STALE;}
 owner_record*r=&records[t.slot];if(r->state!=OWNER_INITIALIZING){owner_unlock();return SPURS_OWNER_STATE;}
 for(unsigned j=0;j<r->nspus;j++)if(!r->contexts[j]){owner_unlock();return SPURS_OWNER_STATE;}
 r->state=OWNER_ACTIVE;owner_unlock();return SPURS_OWNER_OK;
}
spurs_owner_status spurs_owner_abort(spurs_owner_token t)
{
 if(!valid_token(t))return SPURS_OWNER_INVALID;
 owner_lock();if(!exact_token(t)){owner_unlock();return SPURS_OWNER_STALE;}
 owner_record*r=&records[t.slot];if(r->state!=OWNER_INITIALIZING){owner_unlock();return SPURS_OWNER_STATE;}
 if(r->refs||has_leases(r)){owner_unlock();return SPURS_OWNER_BUSY;}
 memset(r,0,sizeof*r);owner_unlock();return SPURS_OWNER_OK;
}
spurs_owner_status spurs_owner_retain(uint64_t ea,spurs_owner_token*out)
{
 if(!out||!valid_ea(ea))return SPURS_OWNER_INVALID;
 owner_lock();for(unsigned i=0;i<SPURS_OWNER_LIMIT;i++)if(records[i].state!=OWNER_EMPTY&&records[i].token.ea==ea){
  owner_record*r=&records[i];if(r->state!=OWNER_ACTIVE){owner_unlock();return SPURS_OWNER_STATE;}
  if(r->refs==UINT64_MAX){owner_unlock();return SPURS_OWNER_FULL;}
  ++r->refs;*out=r->token;owner_unlock();return SPURS_OWNER_OK;
 }
 owner_unlock();return SPURS_OWNER_STALE;
}
spurs_owner_status spurs_owner_release(spurs_owner_token t)
{
 if(!valid_token(t))return SPURS_OWNER_INVALID;
 owner_lock();if(!exact_token(t)){owner_unlock();return SPURS_OWNER_STALE;}
 owner_record*r=&records[t.slot];if(r->state!=OWNER_ACTIVE||!r->refs){owner_unlock();return SPURS_OWNER_STATE;}
 --r->refs;owner_unlock();return SPURS_OWNER_OK;
}
spurs_owner_status spurs_owner_acquire_worker(spurs_owner_token t,uint32_t index,spurs_worker_lease*out)
{
 if(!out||!valid_token(t)||index>=SPURS_OWNER_WORKER_LIMIT)return SPURS_OWNER_INVALID;
 owner_lock();if(!exact_token(t)){owner_unlock();return SPURS_OWNER_STALE;}
 owner_record*r=&records[t.slot];if(r->state!=OWNER_ACTIVE){owner_unlock();return SPURS_OWNER_STATE;}
 if(index>=r->nspus){owner_unlock();return SPURS_OWNER_INVALID;}
 if(r->cookies[index]){owner_unlock();return SPURS_OWNER_BUSY;}
 if(next_cookie==UINT64_MAX){owner_unlock();return SPURS_OWNER_FULL;}
 spurs_worker_lease lease;memset(&lease,0,sizeof lease);lease.owner=t;lease.index=index;
 lease.cookie=next_cookie++;lease.context=r->contexts[index];r->cookies[index]=lease.cookie;*out=lease;
 owner_unlock();return SPURS_OWNER_OK;
}
static spurs_owner_status check_lease(spurs_worker_lease l)
{
 if(!valid_token(l.owner)||!l.context||!l.cookie||l.index>=SPURS_OWNER_WORKER_LIMIT)return SPURS_OWNER_INVALID;
 if(!exact_token(l.owner))return SPURS_OWNER_STALE;
 owner_record*r=&records[l.owner.slot];if(r->state!=OWNER_ACTIVE)return SPURS_OWNER_STATE;
 if(l.index>=r->nspus)return SPURS_OWNER_INVALID;
 if(r->cookies[l.index]!=l.cookie||r->contexts[l.index]!=l.context)return SPURS_OWNER_STALE;
 return SPURS_OWNER_OK;
}
spurs_owner_status spurs_owner_validate_worker(spurs_worker_lease l)
{owner_lock();spurs_owner_status s=check_lease(l);owner_unlock();return s;}
spurs_owner_status spurs_owner_release_worker(spurs_worker_lease l)
{owner_lock();spurs_owner_status s=check_lease(l);if(s==SPURS_OWNER_OK)records[l.owner.slot].cookies[l.index]=0;owner_unlock();return s;}
spurs_owner_status spurs_owner_close_begin(spurs_owner_token t)
{
 if(!valid_token(t))return SPURS_OWNER_INVALID;
 owner_lock();if(!exact_token(t)){owner_unlock();return SPURS_OWNER_STALE;}
 owner_record*r=&records[t.slot];if(r->state!=OWNER_ACTIVE){owner_unlock();return SPURS_OWNER_STATE;}
 if(r->refs||has_leases(r)){owner_unlock();return SPURS_OWNER_BUSY;}
 r->state=OWNER_CLOSING;owner_unlock();return SPURS_OWNER_OK;
}
spurs_owner_status spurs_owner_close_cancel(spurs_owner_token t)
{
 if(!valid_token(t))return SPURS_OWNER_INVALID;
 owner_lock();if(!exact_token(t)){owner_unlock();return SPURS_OWNER_STALE;}
 owner_record*r=&records[t.slot];if(r->state!=OWNER_CLOSING){owner_unlock();return SPURS_OWNER_STATE;}
 r->state=OWNER_ACTIVE;owner_unlock();return SPURS_OWNER_OK;
}
spurs_owner_status spurs_owner_close_finish(spurs_owner_token t,void**contexts,uint32_t capacity,uint32_t*count)
{
 if(!valid_token(t)||!contexts||!count)return SPURS_OWNER_INVALID;
 owner_lock();if(!exact_token(t)){owner_unlock();return SPURS_OWNER_STALE;}
 owner_record*r=&records[t.slot];if(r->state!=OWNER_CLOSING){owner_unlock();return SPURS_OWNER_STATE;}
 if(r->refs||has_leases(r)){owner_unlock();return SPURS_OWNER_BUSY;}
 if(capacity<r->nspus){owner_unlock();return SPURS_OWNER_FULL;}
 uint32_t n=r->nspus;memcpy(contexts,r->contexts,n*sizeof contexts[0]);*count=n;
 memset(r,0,sizeof*r);owner_unlock();return SPURS_OWNER_OK;
}

#include "spurs_event_ports.h"
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK registry_mutex=SRWLOCK_INIT;
static void lock_registry(void){AcquireSRWLockExclusive(&registry_mutex);}
static void unlock_registry(void){ReleaseSRWLockExclusive(&registry_mutex);}
#else
#include <pthread.h>
static pthread_mutex_t registry_mutex=PTHREAD_MUTEX_INITIALIZER;
static void lock_registry(void){pthread_mutex_lock(&registry_mutex);}
static void unlock_registry(void){pthread_mutex_unlock(&registry_mutex);}
#endif
typedef struct event_owner {uint32_t owner;uint32_t queues[SPURS_EVENT_PORT_COUNT];} event_owner;
static event_owner owners[SPURS_EVENT_PORT_OWNER_LIMIT];
/* Every helper below requires registry_mutex. No callback or guest access. */
static int find_owner(uint32_t owner)
{for(unsigned i=0;i<SPURS_EVENT_PORT_OWNER_LIMIT;i++)if(owners[i].owner==owner)return (int)i;return -1;}
static int contains(const uint32_t *v,uint32_t n,uint32_t q)
{for(uint32_t i=0;i<n;i++)if(v[i]==q)return 1;return 0;}
static int used_elsewhere(int excluded,uint32_t q)
{
 for(unsigned i=0;i<SPURS_EVENT_PORT_OWNER_LIMIT;i++)if((int)i!=excluded&&owners[i].owner)
  for(unsigned p=0;p<SPURS_EVENT_PORT_COUNT;p++)if(owners[i].queues[p]==q)return 1;
 return 0;
}
spurs_event_port_status spurs_event_ports_attach(uint32_t owner,uint32_t queue,
 uint32_t requested_port,int is_dynamic,uint32_t *selected)
{
 if(!owner||!queue||!selected||requested_port>=SPURS_EVENT_PORT_COUNT)return SPURS_EVENT_PORT_INVALID;
 lock_registry();int i=find_owner(owner),fresh=0;
 if(i<0){for(i=0;i<(int)SPURS_EVENT_PORT_OWNER_LIMIT;i++)if(!owners[i].owner)break;
  if(i==(int)SPURS_EVENT_PORT_OWNER_LIMIT){unlock_registry();return SPURS_EVENT_PORT_FULL;}fresh=1;}
 uint32_t port=requested_port;
 if(is_dynamic){port=SPURS_EVENT_PORT_COUNT;
  for(unsigned n=0;n<48;n++){unsigned p=n+16;
   if(!owners[i].queues[p]){port=p;break;}}
  if(port==SPURS_EVENT_PORT_COUNT){unlock_registry();return SPURS_EVENT_PORT_BUSY;}
 } else if(owners[i].queues[port]){unlock_registry();return SPURS_EVENT_PORT_BUSY;}
 if(fresh)owners[i].owner=owner;
 owners[i].queues[port]=queue;*selected=port;unlock_registry();return SPURS_EVENT_PORT_OK;
}
spurs_event_port_status spurs_event_ports_detach(uint32_t owner,uint32_t port)
{
 if(!owner||port>=SPURS_EVENT_PORT_COUNT)return SPURS_EVENT_PORT_INVALID;
 lock_registry();int i=find_owner(owner);
 if(i<0||!owners[i].queues[port]){unlock_registry();return SPURS_EVENT_PORT_NOT_FOUND;}
 owners[i].queues[port]=0;unsigned p=0;
 while(p<SPURS_EVENT_PORT_COUNT&&!owners[i].queues[p])++p;
 if(p==SPURS_EVENT_PORT_COUNT)owners[i].owner=0;
 unlock_registry();return SPURS_EVENT_PORT_OK;
}
uint32_t spurs_event_ports_lookup(uint32_t owner,uint32_t port)
{
 if(!owner||port>=SPURS_EVENT_PORT_COUNT)return 0;
 lock_registry();int i=find_owner(owner);uint32_t q=i<0?0:owners[i].queues[port];unlock_registry();return q;
}
uint32_t spurs_event_ports_lookup_unique(uint32_t port)
{
 if(port>=SPURS_EVENT_PORT_COUNT)return 0;
 lock_registry();uint32_t q=0;unsigned matches=0;
 for(unsigned i=0;i<SPURS_EVENT_PORT_OWNER_LIMIT;i++)if(owners[i].owner&&owners[i].queues[port]){q=owners[i].queues[port];++matches;}
 unlock_registry();return matches==1?q:0;
}
uint32_t spurs_event_ports_remove_queue(uint32_t queue)
{
 if(!queue)return 0;
 lock_registry();uint32_t removed=0;
 for(unsigned i=0;i<SPURS_EVENT_PORT_OWNER_LIMIT;i++)if(owners[i].owner){
  unsigned remaining=0;
  for(unsigned p=0;p<SPURS_EVENT_PORT_COUNT;p++){
   if(owners[i].queues[p]==queue){owners[i].queues[p]=0;++removed;}
   if(owners[i].queues[p])++remaining;
  }
  if(!remaining)owners[i].owner=0;
 }
 unlock_registry();return removed;
}
spurs_event_port_status spurs_event_ports_clear_owner(uint32_t owner,
 uint32_t *orphaned_queues,uint32_t capacity,uint32_t *count)
{
 if(!owner||!orphaned_queues||!count)return SPURS_EVENT_PORT_INVALID;
 lock_registry();int i=find_owner(owner);
 if(i<0){unlock_registry();return SPURS_EVENT_PORT_NOT_FOUND;}
 uint32_t result[SPURS_EVENT_PORT_COUNT],n=0;
 for(unsigned p=0;p<SPURS_EVENT_PORT_COUNT;p++){uint32_t q=owners[i].queues[p];
  if(q&&!contains(result,n,q)&&!used_elsewhere(i,q))result[n++]=q;}
 if(n>capacity){unlock_registry();return SPURS_EVENT_PORT_FULL;}
 memset(&owners[i],0,sizeof owners[i]);
 if(n)memcpy(orphaned_queues,result,n*sizeof result[0]);
 *count=n;
 unlock_registry();return SPURS_EVENT_PORT_OK;
}
spurs_event_port_status spurs_event_ports_snapshot_all(uint32_t *queues,
 uint32_t capacity,uint32_t *count)
{
 if(!queues||!count)return SPURS_EVENT_PORT_INVALID;
 lock_registry();uint32_t result[SPURS_EVENT_PORT_OWNER_LIMIT*SPURS_EVENT_PORT_COUNT],n=0;
 for(unsigned i=0;i<SPURS_EVENT_PORT_OWNER_LIMIT;i++)if(owners[i].owner)
  for(unsigned p=0;p<SPURS_EVENT_PORT_COUNT;p++){uint32_t q=owners[i].queues[p];if(q&&!contains(result,n,q))result[n++]=q;}
 if(n>capacity){unlock_registry();return SPURS_EVENT_PORT_FULL;}
 if(n)memcpy(queues,result,n*sizeof result[0]);
 *count=n;
 unlock_registry();return SPURS_EVENT_PORT_OK;
}

/* Synthetic host-pointer bookkeeping only. Compile with the real module,
 * C17 and -pthread on POSIX; no HLE/runtime replacements or game execution. */
#include "spurs_owner_lifetime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif
#define T(x) do{if(!(x)){fprintf(stderr,"OWNER_LIFE_FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#define OK SPURS_OWNER_OK
static unsigned contexts[24];
static spurs_owner_token reserve(uint64_t ea,unsigned n){spurs_owner_token t;T(spurs_owner_reserve(ea,n,&t)==OK);return t;}
static spurs_owner_token active(uint64_t ea,unsigned n,unsigned offset)
{spurs_owner_token t=reserve(ea,n);for(unsigned i=0;i<n;i++)T(spurs_owner_bind_worker(t,i,&contexts[offset+i])==OK);T(spurs_owner_publish(t)==OK);return t;}
static void close_owner(spurs_owner_token t,unsigned n,unsigned offset)
{void*p[6];uint32_t count=99;T(spurs_owner_close_begin(t)==OK);T(spurs_owner_close_finish(t,p,6,&count)==OK&&count==n);for(unsigned i=0;i<n;i++)T(p[i]==&contexts[offset+i]);}
static void lifecycle(void)
{
 spurs_owner_token t=reserve(0x10000,2),out={0x20000,3,999},saved=out;
 T(spurs_owner_retain(0x10000,&out)==SPURS_OWNER_STATE&&!memcmp(&out,&saved,sizeof out));
 T(spurs_owner_publish(t)==SPURS_OWNER_STATE);T(spurs_owner_bind_worker(t,0,&contexts[0])==OK);
 T(spurs_owner_publish(t)==SPURS_OWNER_STATE);T(spurs_owner_bind_worker(t,1,&contexts[1])==OK);T(spurs_owner_publish(t)==OK);
 T(spurs_owner_retain(0x10000,&out)==OK&&out.generation==t.generation);
 spurs_worker_lease lease,failed;memset(&failed,0xA5,sizeof failed);spurs_worker_lease sentinel=failed;
 T(spurs_owner_acquire_worker(t,0,&lease)==OK&&lease.context==&contexts[0]);T(spurs_owner_validate_worker(lease)==OK);
 T(spurs_owner_acquire_worker(t,0,&failed)==SPURS_OWNER_BUSY&&!memcmp(&failed,&sentinel,sizeof failed));
 T(spurs_owner_close_begin(t)==SPURS_OWNER_BUSY);T(spurs_owner_release(out)==OK);T(spurs_owner_close_begin(t)==SPURS_OWNER_BUSY);
 T(spurs_owner_release_worker(lease)==OK);T(spurs_owner_close_begin(t)==OK);
 T(spurs_owner_retain(t.ea,&out)==SPURS_OWNER_STATE);T(spurs_owner_acquire_worker(t,0,&failed)==SPURS_OWNER_STATE&&!memcmp(&failed,&sentinel,sizeof failed));
 void*p[6];for(unsigned i=0;i<6;i++)p[i]=&contexts[20];uint32_t count=99;
 T(spurs_owner_close_finish(t,p,1,&count)==SPURS_OWNER_FULL&&count==99);for(unsigned i=0;i<6;i++)T(p[i]==&contexts[20]);
 T(spurs_owner_close_finish(t,p,6,&count)==OK&&count==2&&p[0]==&contexts[0]&&p[1]==&contexts[1]);
 T(spurs_owner_publish(t)==SPURS_OWNER_STALE&&spurs_owner_release(t)==SPURS_OWNER_STALE);
}
static void reuse(void)
{
 spurs_owner_token old=reserve(0x10000,1);T(spurs_owner_bind_worker(old,0,&contexts[0])==OK);T(spurs_owner_abort(old)==OK);
 spurs_owner_token now=active(0x10000,1,0);T(now.slot==old.slot&&now.generation!=old.generation);
 T(spurs_owner_abort(old)==SPURS_OWNER_STALE&&spurs_owner_bind_worker(old,0,&contexts[1])==SPURS_OWNER_STALE);
 close_owner(now,1,0);spurs_owner_token next=active(0x10000,1,0);T(next.generation!=now.generation);T(spurs_owner_close_begin(now)==SPURS_OWNER_STALE);close_owner(next,1,0);
}
static void stalelease(void)
{
 spurs_owner_token t=active(0x10000,1,0);spurs_worker_lease old,fresh;
 T(spurs_owner_acquire_worker(t,0,&old)==OK);T(spurs_owner_release_worker(old)==OK);T(spurs_owner_acquire_worker(t,0,&fresh)==OK&&fresh.cookie!=old.cookie);
 T(spurs_owner_release_worker(old)==SPURS_OWNER_STALE&&spurs_owner_validate_worker(fresh)==OK);
 spurs_worker_lease wrong=fresh;wrong.context=&contexts[1];T(spurs_owner_release_worker(wrong)==SPURS_OWNER_STALE);
 wrong=fresh;wrong.cookie^=UINT64_C(0x8000000000000000);T(spurs_owner_release_worker(wrong)==SPURS_OWNER_STALE);
 T(spurs_owner_validate_worker(fresh)==OK&&spurs_owner_release_worker(fresh)==OK&&spurs_owner_release_worker(fresh)==SPURS_OWNER_STALE);
 close_owner(t,1,0);spurs_owner_token n=active(0x10000,1,0);T(spurs_owner_acquire_worker(n,0,&fresh)==OK);
 T(spurs_owner_release_worker(old)==SPURS_OWNER_STALE&&spurs_owner_validate_worker(fresh)==OK);T(spurs_owner_release_worker(fresh)==OK);close_owner(n,1,0);
}
static void sharedcontext(void)
{
 spurs_owner_token a=reserve(0x10000,2),b=reserve(0x11000,1);
 T(spurs_owner_bind_worker(a,0,&contexts[0])==OK);T(spurs_owner_bind_worker(a,1,&contexts[0])==SPURS_OWNER_BUSY);
 T(spurs_owner_bind_worker(b,0,&contexts[0])==SPURS_OWNER_BUSY);T(spurs_owner_abort(a)==OK);
 T(spurs_owner_bind_worker(b,0,&contexts[0])==OK&&spurs_owner_publish(b)==OK);close_owner(b,1,0);
}
static void reject(void)
{
 spurs_owner_token t={0},out;memset(&out,0xA5,sizeof out);spurs_owner_token before=out;
 uint64_t bad[]={0,0xFFFF,0x10001,UINT64_C(0x100010000)};
 for(unsigned i=0;i<4;i++)T(spurs_owner_reserve(bad[i],1,&out)==SPURS_OWNER_INVALID&&!memcmp(&out,&before,sizeof out));
 T(spurs_owner_reserve(0x10000,0,&out)==SPURS_OWNER_INVALID&&spurs_owner_reserve(0x10000,7,&out)==SPURS_OWNER_INVALID);
 T(spurs_owner_reserve(0x10000,1,NULL)==SPURS_OWNER_INVALID&&spurs_owner_publish(t)==SPURS_OWNER_INVALID);
 spurs_worker_lease zero={0};T(spurs_owner_validate_worker(zero)==SPURS_OWNER_INVALID&&spurs_owner_release_worker(zero)==SPURS_OWNER_INVALID);
 spurs_owner_token owners[4];for(unsigned i=0;i<4;i++)owners[i]=reserve(0x10000+128*i,1);
 T(spurs_owner_reserve(0x20000,1,&out)==SPURS_OWNER_FULL&&!memcmp(&out,&before,sizeof out));
 T(spurs_owner_reserve(0x10000,1,&out)==SPURS_OWNER_BUSY&&!memcmp(&out,&before,sizeof out));
 T(spurs_owner_bind_worker(owners[0],0,NULL)==SPURS_OWNER_INVALID&&spurs_owner_bind_worker(owners[0],1,&contexts[0])==SPURS_OWNER_INVALID);
 T(spurs_owner_close_begin(owners[0])==SPURS_OWNER_STATE);for(unsigned i=0;i<4;i++)T(spurs_owner_abort(owners[i])==OK);
}
static void refs(void)
{
 spurs_owner_token t=active(0x10000,6,0),a,b;T(spurs_owner_abort(t)==SPURS_OWNER_STATE);
 T(spurs_owner_release(t)==SPURS_OWNER_STATE);T(spurs_owner_retain(t.ea,&a)==OK&&spurs_owner_retain(t.ea,&b)==OK);
 T(spurs_owner_close_begin(t)==SPURS_OWNER_BUSY);T(spurs_owner_release(a)==OK&&spurs_owner_close_begin(t)==SPURS_OWNER_BUSY);
 T(spurs_owner_release(b)==OK&&spurs_owner_release(b)==SPURS_OWNER_STATE);
 spurs_worker_lease leases[6];for(unsigned i=0;i<6;i++)T(spurs_owner_acquire_worker(t,i,&leases[i])==OK&&leases[i].context==&contexts[i]);
 T(spurs_owner_close_begin(t)==SPURS_OWNER_BUSY);for(unsigned i=0;i<6;i++)T(spurs_owner_release_worker(leases[i])==OK);close_owner(t,6,0);
}
static spurs_owner_token thread_owners[2];static unsigned isolated;
static atomic_uint inside[2],successes[2],busy_count,ready,start_gate;
static void stress(unsigned id)
{
 unsigned slot=isolated?id:0;spurs_owner_token t=thread_owners[slot];
 if(!isolated){spurs_worker_lease blocked;T(spurs_owner_acquire_worker(t,0,&blocked)==SPURS_OWNER_BUSY);atomic_fetch_add(&busy_count,1);}
 atomic_fetch_add_explicit(&ready,1,memory_order_release);while(!atomic_load_explicit(&start_gate,memory_order_acquire)){}
 for(unsigned i=0;i<10000;i++){
  spurs_owner_token held;T(spurs_owner_retain(t.ea,&held)==OK);
  spurs_worker_lease lease;spurs_owner_status s=spurs_owner_acquire_worker(held,0,&lease);
  if(s==OK){T(lease.context==&contexts[slot]);T(atomic_fetch_add_explicit(&inside[slot],1,memory_order_relaxed)==0);
   T(spurs_owner_validate_worker(lease)==OK);atomic_fetch_add_explicit(&successes[id],1,memory_order_relaxed);
   T(atomic_fetch_sub_explicit(&inside[slot],1,memory_order_relaxed)==1);T(spurs_owner_release_worker(lease)==OK);
  }else{T(s==SPURS_OWNER_BUSY);atomic_fetch_add_explicit(&busy_count,1,memory_order_relaxed);}
  T(spurs_owner_release(held)==OK);
 }
}
#ifdef _WIN32
static DWORD WINAPI worker(LPVOID p){stress((unsigned)(uintptr_t)p);return 0;}
typedef HANDLE thread;
static thread start(unsigned id){HANDLE h=CreateThread(NULL,0,worker,(LPVOID)(uintptr_t)id,0,NULL);T(h);return h;}
static void join(thread t){T(WaitForSingleObject(t,30000)==WAIT_OBJECT_0);T(CloseHandle(t));}
#else
static void* worker(void*p){stress((unsigned)(uintptr_t)p);return NULL;}
typedef pthread_t thread;
static thread start(unsigned id){pthread_t t;T(pthread_create(&t,NULL,worker,(void*)(uintptr_t)id)==0);return t;}
static void join(thread t){T(pthread_join(t,NULL)==0);}
#endif
static void concurrent(void)
{
 thread_owners[0]=active(0x10000,1,0);thread_owners[1]=active(0x11000,1,1);
 for(isolated=0;isolated<2;isolated++){
  atomic_store(&successes[0],0);atomic_store(&successes[1],0);atomic_store(&ready,0);atomic_store(&start_gate,0);
  spurs_worker_lease held;if(!isolated)T(spurs_owner_acquire_worker(thread_owners[0],0,&held)==OK);
  thread a=start(0),b=start(1);while(atomic_load_explicit(&ready,memory_order_acquire)!=2){}
  if(!isolated)T(spurs_owner_release_worker(held)==OK);atomic_store_explicit(&start_gate,1,memory_order_release);
  join(a);join(b);T(atomic_load(&successes[0])+atomic_load(&successes[1])>0);
  if(isolated)T(atomic_load(&successes[0])==10000&&atomic_load(&successes[1])==10000);
 }
 close_owner(thread_owners[0],1,0);close_owner(thread_owners[1],1,1);printf("OWNER_LIFE_CONTENTION busy=%u\n",atomic_load(&busy_count));
}
int main(int argc,char**argv)
{
 if(argc!=2)return 2;
 if(!strcmp(argv[1],"lifecycle"))lifecycle();else if(!strcmp(argv[1],"reuse"))reuse();else if(!strcmp(argv[1],"stalelease"))stalelease();
 else if(!strcmp(argv[1],"sharedcontext"))sharedcontext();else if(!strcmp(argv[1],"reject"))reject();else if(!strcmp(argv[1],"refs"))refs();
 else if(!strcmp(argv[1],"concurrent"))concurrent();else return 2;puts("OWNER_LIFE_PASS");return 0;
}

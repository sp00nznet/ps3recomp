/* Production registry unit fixture; synthetic owner/queue IDs only.
 * C17: compile this file with runtime/spu/spurs_event_ports.c, -pthread on
 * POSIX. No HLE replacements and no initialization/queue lifetime claim. */
#include "spurs_event_ports.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif
#define T(x) do{if(!(x)){fprintf(stderr,"PORT_FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define OK SPURS_EVENT_PORT_OK
static void attach(uint32_t o,uint32_t q,uint32_t p){uint32_t s=99;T(spurs_event_ports_attach(o,q,p,0,&s)==OK&&s==p);}
static void clear(uint32_t o){uint32_t q[64],n;T(spurs_event_ports_clear_owner(o,q,64,&n)==OK);}
static void empty(void){uint32_t q=123,n=456;T(spurs_event_ports_snapshot_all(&q,0,&n)==OK&&n==0&&q==123);}
static void basic(void)
{
 empty();attach(1,11,5);attach(2,22,5);
 T(spurs_event_ports_lookup(1,5)==11&&spurs_event_ports_lookup(2,5)==22&&spurs_event_ports_lookup_unique(5)==0);
 uint32_t s=123,n=456,q=789;
 T(spurs_event_ports_attach(1,33,5,0,&s)==SPURS_EVENT_PORT_BUSY&&s==123);
 T(spurs_event_ports_attach(0,33,6,0,&s)==SPURS_EVENT_PORT_INVALID&&s==123);
 T(spurs_event_ports_attach(1,0,6,0,&s)==SPURS_EVENT_PORT_INVALID&&s==123);
 T(spurs_event_ports_attach(1,33,64,1,&s)==SPURS_EVENT_PORT_INVALID&&s==123);
 T(spurs_event_ports_attach(1,33,6,0,NULL)==SPURS_EVENT_PORT_INVALID);
 T(spurs_event_ports_detach(0,5)==SPURS_EVENT_PORT_INVALID&&spurs_event_ports_detach(1,64)==SPURS_EVENT_PORT_INVALID);
 T(spurs_event_ports_detach(1,6)==SPURS_EVENT_PORT_NOT_FOUND&&spurs_event_ports_lookup(1,5)==11);
 T(spurs_event_ports_snapshot_all(NULL,0,&n)==SPURS_EVENT_PORT_INVALID&&n==456);
 T(spurs_event_ports_snapshot_all(&q,1,NULL)==SPURS_EVENT_PORT_INVALID&&q==789);
 T(spurs_event_ports_snapshot_all(&q,1,&n)==SPURS_EVENT_PORT_FULL&&q==789&&n==456);
 T(spurs_event_ports_clear_owner(1,NULL,0,&n)==SPURS_EVENT_PORT_INVALID&&n==456);
 T(spurs_event_ports_clear_owner(1,&q,1,NULL)==SPURS_EVENT_PORT_INVALID&&q==789);
 T(spurs_event_ports_lookup(0,5)==0&&spurs_event_ports_lookup(1,64)==0&&spurs_event_ports_lookup_unique(64)==0);
 T(spurs_event_ports_detach(2,5)==OK&&spurs_event_ports_lookup_unique(5)==11);
 attach(2,11,5);T(spurs_event_ports_lookup_unique(5)==0); /* same queue is still ambiguous */
 clear(1);T(spurs_event_ports_lookup_unique(5)==11);clear(2);empty();
}
static void dynamic(void)
{
 uint32_t s=999;
 for(unsigned i=0;i<48;i++){T(spurs_event_ports_attach(1,100+i,0,1,&s)==OK&&s==i+16);}
 s=999;T(spurs_event_ports_attach(1,999,0,1,&s)==SPURS_EVENT_PORT_BUSY&&s==999);
 /* Low ports remain free, but dynamic exhaustion must not fall back there. */
 T(spurs_event_ports_lookup(1,0)==0);
 /* A second owner starts its own high-port search at16. */
 T(spurs_event_ports_attach(2,200,0,1,&s)==OK&&s==16);
 T(spurs_event_ports_detach(1,16)==OK);T(spurs_event_ports_attach(1,999,0,1,&s)==OK&&s==16);
 attach(1,300,0); /* static low port remains separately available */
 clear(1);clear(2);empty();
}
static void clearing(void)
{
 attach(1,100,1);attach(1,101,2);attach(1,101,3);attach(2,100,4);
 uint32_t out[64];for(unsigned i=0;i<64;i++)out[i]=0xABCD0000+i;uint32_t n=999;
 T(spurs_event_ports_clear_owner(1,out,0,&n)==SPURS_EVENT_PORT_FULL&&n==999);
 for(unsigned i=0;i<64;i++)T(out[i]==0xABCD0000+i);
 T(spurs_event_ports_lookup(1,2)==101&&spurs_event_ports_lookup(2,4)==100);
 T(spurs_event_ports_clear_owner(1,out,64,&n)==OK&&n==1&&out[0]==101&&out[1]==0xABCD0001);
 n=999;out[0]=888;T(spurs_event_ports_clear_owner(1,out,64,&n)==SPURS_EVENT_PORT_NOT_FOUND&&n==999&&out[0]==888);
 T(spurs_event_ports_snapshot_all(out,64,&n)==OK&&n==1&&out[0]==100);
 T(spurs_event_ports_clear_owner(2,out,64,&n)==OK&&n==1&&out[0]==100);empty();
 /* All bindings shared: clearing succeeds with capacity0 and reports none. */
 attach(1,100,1);attach(2,100,1);n=999;out[0]=888;
 T(spurs_event_ports_clear_owner(1,out,0,&n)==OK&&n==0&&out[0]==888);clear(2);empty();
}
static void capacity(void)
{
 for(unsigned o=1;o<=4;o++)attach(o,o,0);
 uint32_t s=777;T(spurs_event_ports_attach(5,5,0,0,&s)==SPURS_EVENT_PORT_FULL&&s==777);
 T(spurs_event_ports_detach(1,0)==OK);attach(5,5,0); /* empty owner slot reclaimed */
 for(unsigned o=2;o<=5;o++)clear(o);empty();
}
static void remove_queue(void)
{
 attach(1,100,1);attach(1,100,2);attach(1,101,3);attach(2,100,4);
 T(spurs_event_ports_remove_queue(0)==0);
 T(spurs_event_ports_remove_queue(100)==3);
 T(spurs_event_ports_lookup(1,1)==0&&spurs_event_ports_lookup(1,2)==0);
 T(spurs_event_ports_lookup(1,3)==101&&spurs_event_ports_lookup(2,4)==0);
 T(spurs_event_ports_remove_queue(100)==0);
 attach(3,102,1);clear(1);clear(3);empty();
}
static unsigned shared_owner;
static void stress(unsigned id)
{
 uint32_t owner=shared_owner?50:50+id,queue=500+id;
 for(unsigned i=0;i<10000;i++) {
  uint32_t p=999;T(spurs_event_ports_attach(owner,queue,7,shared_owner,&p)==OK);
  T(spurs_event_ports_lookup(owner,p)==queue);
  uint32_t all[256],n=999;T(spurs_event_ports_snapshot_all(all,256,&n)==OK&&n>=1&&n<=2);
  for(unsigned k=0;k<n;k++)T(all[k]==501||all[k]==502);
  T(spurs_event_ports_detach(owner,p)==OK);
 }
}
#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p){stress((unsigned)(uintptr_t)p);return 0;}
typedef HANDLE thread;
static thread start(unsigned id){HANDLE h=CreateThread(NULL,0,thread_main,(LPVOID)(uintptr_t)id,0,NULL);T(h);return h;}
static void join(thread t){T(WaitForSingleObject(t,60000)==WAIT_OBJECT_0);T(CloseHandle(t));}
#else
static void* thread_main(void*p){stress((unsigned)(uintptr_t)p);return NULL;}
typedef pthread_t thread;
static thread start(unsigned id){pthread_t t;T(pthread_create(&t,NULL,thread_main,(void*)(uintptr_t)id)==0);return t;}
static void join(thread t){T(pthread_join(t,NULL)==0);}
#endif
static void concurrent(void)
{for(shared_owner=0;shared_owner<2;shared_owner++){thread a=start(1),b=start(2);join(a);join(b);empty();}}
int main(int argc,char**argv)
{
 if(argc!=2)return 2;
 if(!strcmp(argv[1],"basic"))basic();else if(!strcmp(argv[1],"dynamic"))dynamic();
 else if(!strcmp(argv[1],"clear"))clearing();else if(!strcmp(argv[1],"capacity"))capacity();
 else if(!strcmp(argv[1],"remove-queue"))remove_queue();
 else if(!strcmp(argv[1],"concurrent"))concurrent();else if(!strcmp(argv[1],"all")){basic();dynamic();clearing();capacity();remove_queue();concurrent();}
 else return 2;puts("PORT_PASS");return 0;
}

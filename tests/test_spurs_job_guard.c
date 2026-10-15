/* Synthetic production guard tests. Compile this TU instead of cellSpurs.c;
 * link the genuine runtime libraries, never replacement guard/VM/strict funcs.
 * Each mode runs in a fresh process. timeout/unknown-strict expect exit86. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../libs/spurs/cellSpurs.c"
#include <stdatomic.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"GUARD_FAIL line=%d %s\n",__LINE__,#x); exit(1); } } while(0)
#define GUARD_EA 0x10000u
#define PARAM_EA 0x20000u
#define ROUNDS 2000u
static atomic_uint acknowledged;
static unsigned worker_kind;
static void set_strict(int enabled)
{
#ifdef _WIN32
    CHECK(_putenv_s("PS3_STRICT_BOOT",enabled?"1":"")==0);
#else
    CHECK(enabled?setenv("PS3_STRICT_BOOT","1",1)==0:unsetenv("PS3_STRICT_BOOT")==0);
#endif
}
static void worker(unsigned index)
{
    if(worker_kind==0) {
        for(unsigned n=1;n<=ROUNDS;n++) {
            while(atomic_load_explicit(&acknowledged,memory_order_acquire)!=n-1) Sleep(0);
            /* Plain guest stores: only Notify/wait publishes these to reader. */
            vm_write32(PARAM_EA,n);vm_write32(PARAM_EA+4,~n);
            CHECK(cellSpursJobGuardNotify(GUARD_EA)==CELL_OK);
        }
    } else if(worker_kind==1) {
        for(unsigned n=0;n<ROUNDS;n++) CHECK(cellSpursJobGuardNotify(GUARD_EA)==CELL_OK);
    } else {
        for(unsigned n=0;n<16;n++) {
            u32 ea=GUARD_EA+(index*16+n)*128;
            CHECK(cellSpursJobGuardInitialize(0x30000,ea,1,1,1)==CELL_OK);
            CHECK(cellSpursJobGuardNotify(ea)==CELL_OK);
        }
    }
}
#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p){worker((unsigned)(uintptr_t)p);return 0;}
typedef HANDLE test_thread;
static test_thread start(unsigned n){HANDLE h=CreateThread(NULL,0,thread_main,(LPVOID)(uintptr_t)n,0,NULL);CHECK(h);return h;}
static void join(test_thread t){CHECK(WaitForSingleObject(t,30000)==WAIT_OBJECT_0);CHECK(CloseHandle(t));}
#else
static void* thread_main(void*p){worker((unsigned)(uintptr_t)p);return NULL;}
typedef pthread_t test_thread;
static test_thread start(unsigned n){pthread_t t;CHECK(pthread_create(&t,NULL,thread_main,(void*)(uintptr_t)n)==0);return t;}
static void join(test_thread t){CHECK(pthread_join(t,NULL)==0);}
#endif
int main(int argc,char**argv)
{
    if(argc!=2)return 2;
    CHECK(vm_base==NULL);vm_base=(uint8_t*)calloc(1,0x40000);CHECK(vm_base);
    set_strict(strcmp(argv[1],"unknown-compat")!=0);
    if(!strcmp(argv[1],"publication")) {
        CHECK(cellSpursJobGuardInitialize(0x30000,GUARD_EA,1,1,1)==CELL_OK);
        worker_kind=0;test_thread t=start(0);
        for(unsigned n=1;n<=ROUNDS;n++) {
            CHECK(jg_wait(GUARD_EA)==1);
            CHECK(vm_read32(PARAM_EA)==n && vm_read32(PARAM_EA+4)==~n);
            atomic_store_explicit(&acknowledged,n,memory_order_release);
        }
        join(t);CHECK(vm_read32(GUARD_EA)==1);
    } else if(!strcmp(argv[1],"credits")) {
        CHECK(cellSpursJobGuardInitialize(0x30000,GUARD_EA,2,1,1)==CELL_OK);
        worker_kind=1;test_thread a=start(0),b=start(1);join(a);join(b);
        CHECK(vm_read32(GUARD_EA)==0);
        for(unsigned n=0;n<ROUNDS;n++)CHECK(jg_wait(GUARD_EA)==1);
        CHECK(vm_read32(GUARD_EA)==2);
        CHECK(cellSpursJobGuardNotify(GUARD_EA)==0);
        CHECK(cellSpursJobGuardReset(GUARD_EA)==0 && vm_read32(GUARD_EA)==2);
        CHECK(cellSpursJobGuardNotify(GUARD_EA)==0 && jg_wait(GUARD_EA)==1);
    } else if(!strcmp(argv[1],"registry")) {
        worker_kind=2;test_thread a=start(0),b=start(1);join(a);join(b);
        for(unsigned n=0;n<32;n++)CHECK(jg_wait(GUARD_EA+n*128)==1);
        CHECK(cellSpursJobGuardInitialize(0x30000,GUARD_EA+4096,1,1,1)==CELL_SPURS_TASK_ERROR_AGAIN);
    } else if(!strcmp(argv[1],"one-shot")) {
        CHECK(cellSpursJobGuardInitialize(0x30000,GUARD_EA,1,1,0)==0);
        CHECK(cellSpursJobGuardNotify(GUARD_EA)==0 && jg_wait(GUARD_EA)==1);
        CHECK(jg_wait(GUARD_EA)==1 && cellSpursJobGuardReset(GUARD_EA)==0 && jg_wait(GUARD_EA)==1);
        CHECK(vm_read32(GUARD_EA)==0);
    } else if(!strcmp(argv[1],"overflow")) {
        CHECK(cellSpursJobGuardInitialize(0x30000,GUARD_EA,1,1,1)==0);
        jg_lock();int i=jg_find(GUARD_EA);CHECK(i>=0);
        s_jobguards[i].pending=UINT64_MAX;
        u32 mirrored=vm_read32(GUARD_EA);jg_unlock();
        CHECK(cellSpursJobGuardNotify(GUARD_EA)==CELL_SPURS_TASK_ERROR_AGAIN);
        jg_lock();CHECK(s_jobguards[i].pending==UINT64_MAX);
        CHECK(vm_read32(GUARD_EA)==mirrored);jg_unlock();
    } else if(!strcmp(argv[1],"unknown-strict")) {
        (void)jg_wait(GUARD_EA);return 91;
    } else if(!strcmp(argv[1],"unknown-compat")) {
        CHECK(cellSpursJobGuardNotify(GUARD_EA)==CELL_SPURS_TASK_ERROR_INVAL);
        CHECK(cellSpursJobGuardReset(GUARD_EA)==CELL_SPURS_TASK_ERROR_INVAL);
        CHECK(jg_wait(GUARD_EA)==1);
    } else if(!strcmp(argv[1],"timeout")) {
        CHECK(cellSpursJobGuardInitialize(0x30000,GUARD_EA,2,1,1)==0);
        CHECK(cellSpursJobGuardNotify(GUARD_EA)==0);
        (void)jg_wait(GUARD_EA);return 91;
    } else return 2;
    free(vm_base);vm_base=NULL;puts("GUARD_PASS");return 0;
}

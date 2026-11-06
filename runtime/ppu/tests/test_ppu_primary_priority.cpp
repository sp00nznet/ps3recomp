// Real ELF loader, entry dispatch, main registration and guest priority query.
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include "win32_compat.h"
extern "C" {
uint8_t* vm_base;
// Buffering is covered by its own fixture; this test owns no logger thread.
void ps3_stdio_init(void) {}
uint32_t ppu_hle_inject_base=0x20000000;
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) { std::abort(); }
void spu_lockline_lock(void) { std::abort(); }
void spu_lockline_unlock(void) { std::abort(); }
int spu_raw_reg_load(uint32_t, uint32_t*) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
void cellGcm_ref_on_poll(void) { std::abort(); }
uint32_t g_gcm_consumer_put_wake_ea = 0;
void cellGcm_fifo_kick_notify(void) { std::abort(); }
void sys_memory_set_image_query(uint64_t (*)(uint64_t, uint64_t)) {}
HANDLE CreateThread(void*, SIZE_T, PS3_THREAD_FN, LPVOID, DWORD, DWORD*) { std::abort(); }
BOOL CloseHandle(HANDLE) { std::abort(); }
DWORD GetFileAttributesA(const char*) { std::abort(); }
uint32_t ppu_load_elf(const char*);
int ppu_run(uint32_t, uint32_t);
extern uint32_t ppu_vm_size;
int64_t sys_ppu_thread_get_priority(ppu_context*);
int64_t sys_ppu_thread_set_priority(ppu_context*);
uint64_t ppu_thread_register_main(void);
}
static unsigned failures, called;
static int expected;
static void entry(ppu_context* ctx) {
    ++called;
    if(ctx->thread_id!=1) { std::fprintf(stderr,"FAIL: main identity\n"); ++failures; }
    ppu_context query={}; query.gpr[3]=ctx->thread_id; query.gpr[4]=0x20000;
    int64_t rc=sys_ppu_thread_get_priority(&query);
    int actual=(int32_t)vm_read32(0x20000);
    if(rc || actual!=expected) {
        std::fprintf(stderr,"FAIL: primary priority expected=%d actual=%d rc=%lld\n",expected,actual,(long long)rc);
        ++failures;
    }
}
extern const func_entry function_table[]={{0x10000,entry,"entry"},{0,nullptr,nullptr}};
extern const uint64_t function_table_count=1;
int main(int argc,char** argv) {
    if(argc<3 || argc%2!=1)return 2;
    vm_base=(uint8_t*)mmap(nullptr,0xE0000000ull,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(vm_base==MAP_FAILED)return 2;
    ppu_vm_size=0xE0000000u; ppu_recomp_register();
    // Host bootstrap may register main before the executable is loaded.
    if(ppu_thread_register_main()!=1)return 2;
    ppu_context query={}; query.gpr[3]=1; query.gpr[4]=0x20000;
    if(sys_ppu_thread_get_priority(&query) || vm_read32(0x20000)!=1001)return 1;
    // Repeating the legacy registration must preserve a guest priority update.
    query.gpr[4]=123; if(sys_ppu_thread_set_priority(&query))return 2;
    if(ppu_thread_register_main()!=1)return 2;
    query.gpr[4]=0x20000;
    if(sys_ppu_thread_get_priority(&query) || vm_read32(0x20000)!=123)return 1;
    for(int i=1;i<argc;i+=2) {
        expected=std::atoi(argv[i+1]);
        uint32_t opd=ppu_load_elf(argv[i]);
        if(!opd || ppu_run(opd,0xD0100000u)!=0)return 2;
    }
    if(called!=unsigned((argc-1)/2))return 2;
    munmap(vm_base,0xE0000000ull);
    return failures?1:0;
}

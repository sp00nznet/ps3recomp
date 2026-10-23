#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>
#include "win32_compat.h"
extern "C" {
uint8_t* vm_base;
// This loader fixture has no guest threads, SPUs, RSX, or diagnostic watchers.
// Any attempt to use those subsystems is a test failure, not a success stub.
uint32_t ppu_hle_inject_base=0x20000000;
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) { std::abort(); }
void spu_lockline_lock(void) { std::abort(); }
void spu_lockline_unlock(void) { std::abort(); }
int spu_raw_reg_load(uint32_t, uint32_t*) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
void cellGcm_ref_on_poll(void) { std::abort(); }
// ppu_run buffers host stdio first (runtime/platform/ps3_stdio.c, which has its
// own test); how this fixture's output is buffered does not matter here.
void ps3_stdio_init(void) {}
HANDLE CreateThread(void*, SIZE_T, PS3_THREAD_FN, LPVOID, DWORD, DWORD*) { std::abort(); }
BOOL CloseHandle(HANDLE) { std::abort(); }
DWORD GetFileAttributesA(const char*) { std::abort(); }
uint32_t ppu_load_elf(const char*);
int ppu_run(uint32_t, uint32_t);
extern uint32_t ppu_vm_size;
uint64_t ppu_thread_register_main(void) { return 1; }
void (*g_ppu_thread_entry_trampoline)(uint32_t, uint64_t, uint32_t, uint64_t);
}
static unsigned failures;
static unsigned called;
static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",what); ++failures; }
}
static void entry(ppu_context* ctx) {
    ++called;
    check(vm_read32(0x00B00000)==0x51525354,"entry arguments overwrote PT_LOAD sentinel at old scratch address");
    uint32_t argc=ctx->gpr[3],argv=ctx->gpr[4],envp=ctx->gpr[5];
    check(argv >= ctx->gpr[1]+0x80 && argv < 0xD0100000u,
          "arguments not protected above the entry frame");
    // Simulate a callee prologue and writes to its caller's ABI save area.
    for(unsigned off=0;off<0x80;off+=8) vm_write64(ctx->gpr[1]+off,~uint64_t(0));
    for(unsigned off=8;off<=0x100;off+=8) vm_write64(ctx->gpr[1]-off,~uint64_t(0));
    check(vm_read32(0x40000000)==0x61626364,"heap reservation overwritten");
    check(vm_read32(0xD0100000)==0x71727374,"storage exceeds reserved stack top");
    const char* expected[]={"/dev_bdvd/PS3_GAME/USRDIR/bin/test.self","alpha","beta"};
    unsigned want=std::getenv("PS3_ARGV")?3:1;
    check(argc==want,"argc mismatch");
    for(unsigned i=0;i<argc && i<3;i++) {
        check(vm_read32(argv+i*8)==0,"argv high word not zero");
        uint32_t ptr=vm_read32(argv+i*8+4);
        check(ptr && !std::strcmp((char*)vm_base+ptr,expected[i]),"argv string mismatch");
    }
    check(vm_read64(argv+argc*8)==0,"argv not terminated");
    check(envp && vm_read64(envp)==0 && ctx->gpr[6]==0,"empty environment list/count invalid");
    check((ctx->gpr[1]&15)==0,"entry stack not aligned");
}
extern const func_entry function_table[]={{0x10000,entry,"entry"},{0,nullptr,nullptr}};
extern const uint64_t function_table_count=1;
int main(int argc,char** argv) {
    if(argc!=3)return 2;
    vm_base=(uint8_t*)mmap(nullptr,0xE0000000ull,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(vm_base==MAP_FAILED)return 2;
    ppu_vm_size=0xE0000000u;
    uint32_t opd=ppu_load_elf(argv[1]);
    if(!opd)return 2;
    ppu_recomp_register();
    vm_write32(0x40000000,0x61626364);
    vm_write32(0xD0100000,0x71727374);
    bool reject=std::strcmp(argv[2],"run")!=0;
    uint32_t top=0xD0100000u;
    if(!std::strcmp(argv[2],"low"))top=0x10020;
    if(!std::strcmp(argv[2],"outside"))top=0xF0000000u;
    int rc=ppu_run(opd,top);
    if(reject)check(rc!=0 && called==0,"overlapping entry storage must fail before guest dispatch");
    else check(rc==0 && called==1,"guest entry was not dispatched");
    munmap(vm_base,0xE0000000ull);
    return failures?1:0;
}

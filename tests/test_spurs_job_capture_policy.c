/* CPU-only: real chain job dispatch, registry, owner lease and snapshot capture.
 * Synthetic image and entry, no game bytes. One fresh process/directory per mode.
 * Unknown/open-registry/multiworker modes preserve capture before strict exit86. */
#include "../libs/spurs/cellSpurs.c"
#include <assert.h>

static const char* mode;
static unsigned entered;
static int same(const char* s){return !strcmp(mode,s);}
static int capture_present(void)
{
    char pattern[4096];
    snprintf(pattern,sizeof pattern,"%s/*.image.bin",getenv("FOLKLORE_SPU_CAPTURE_DIR"));
    WIN32_FIND_DATAA data;HANDLE h=FindFirstFileA(pattern,&data);
    if(h==INVALID_HANDLE_VALUE)return 0;
    FindClose(h);return 1;
}
static void entry(spu_context* ctx)
{
    ++entered;
    /* Full capture must precede guest execution, not merely exist at exit. */
    assert(capture_present()==same("full-known"));
    ctx->pc=0x3FFF0;
}

int main(int argc,char** argv)
{
    if(argc!=3)return 2;
    mode=argv[1];
    assert(_putenv_s("PS3_STRICT_BOOT","1")==0);
    assert(_putenv_s("FOLKLORE_SPU_CAPTURE_DIR",argv[2])==0);
    assert(_putenv_s("FOLKLORE_SPU_CAPTURE_UNREGISTERED_ONLY",same("full-known")?"":"1")==0);
    assert(!capture_present());
    assert(vm_base==NULL);vm_base=calloc(1,0x100000);assert(vm_base);ppu_vm_size=0x100000;
    for(unsigned i=0;i<32768;i++)vm_base[0x40000+i]=(uint8_t)(i*13+1);
    vm_write64(0x30000,0x40000);vm_write16(0x30008,32768/16);
    vm_write16(0x3001C,0x400/16);
    const char sha[]="1111111111111111111111111111111111111111111111111111111111111111";
    spurs_native_definition d={0};
    d.image=vm_base+0x40000;d.image_size=32768;
    d.descriptor_template=vm_base+0x30000;d.descriptor_size=128;
    d.entry=entry;d.entry_pc=0x8000;d.source_sha256=d.build_sha256=sha;
    d.layout=(spurs_native_layout){0x8000,0x6000,0x48F0,128,0x20000,0x400,0x3FFF0};
    const spurs_native_profile* registered=NULL;
    assert(spurs_native_registry_register(&d,&registered)==SPURS_NATIVE_OK);
    if(!same("open-registry"))assert(spurs_native_registry_seal()==SPURS_NATIVE_OK);
    if(same("unknown")||same("unknown-multiworker"))vm_base[0x40000]^=1;
    unsigned nspus=same("unknown-multiworker")?2:1;
    assert(cellSpursInitialize((CellSpurs*)(uintptr_t)0x10000,nspus,100,100,1)==CELL_OK);
    struct SpursChain chain={0};chain.jc_ea=0x18000;
    assert(spurs_owner_retain(0x10000,&chain.owner)==SPURS_OWNER_OK);
    spurs_worker_lease held={0};
    if(same("known-busy"))assert(spurs_owner_acquire_worker(chain.owner,0,&held)==SPURS_OWNER_OK);
    int ok=jc_run_one_job(&chain,0x30000,0,128,NULL);
    /* Rejected cases must have already stopped with exit86. */
    assert(same("known")||same("full-known"));
    assert(ok==1&&entered==1&&capture_present()==same("full-known"));
    assert(spurs_owner_release(chain.owner)==SPURS_OWNER_OK);
    int result;unsigned tries=0;
    do{result=cellSpursFinalize((CellSpurs*)(uintptr_t)0x10000);
       if(result==CELL_SPURS_CORE_ERROR_BUSY){assert(++tries<5000);Sleep(1);}
    }while(result==CELL_SPURS_CORE_ERROR_BUSY);
    assert(result==CELL_OK);free(vm_base);vm_base=NULL;
    puts("CAPTURE_POLICY_PASS");return 0;
}

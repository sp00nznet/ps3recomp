/* Run the actual producer callback against a delayed consumer. CPU-only. */
#include "../libs/video/cellGcmSys.c"
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
uint8_t* vm_base;
uint32_t ppu_vm_size=0x200000;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea;return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea,uint64_t value,int width)
{ (void)ea;(void)value;(void)width; }
#define CHECK(x) do {if(!(x)){fprintf(stderr,"refill failed line %d\n",__LINE__);exit(1);}}while(0)
static const u32 ctx=0x10000,begin=0x100000,end=0x1FF000,current=0x1FEF00;
static void prepare(void)
{
    vm_write32(ctx,begin);vm_write32(ctx+4,end);vm_write32(ctx+8,current);
    atomic_store(&g_gcm_fifo_drained_ea,begin+4);
    cellGcmSetHostPresentationSuspended(0);
}
static void* consume(void* unused)
{
    (void)unused;
    /* Exceed the previous two-second deadline while still making progress.
     * The producer must retain its cursor throughout, not reuse this space. */
    for(unsigned i=1;i<=6;i++) {
        usleep(500000);
        CHECK(vm_read32(ctx+8)==current);
        if(i==1) CHECK(vm_read32(GCM_CONTROL_GUEST_ADDR)==current);
        atomic_store_explicit(&g_gcm_fifo_drained_ea,begin+4+i*4,memory_order_release);
    }
    atomic_store_explicit(&g_gcm_fifo_drained_ea,begin,memory_order_release);
    return NULL;
}
static void* restore_and_consume(void* unused)
{
    (void)unused;
    for(unsigned i=0;i<6;i++) {
        usleep(1000000);
        CHECK(vm_read32(ctx+8)==current);
    }
    cellGcmSetHostPresentationSuspended(0);
    atomic_store_explicit(&g_gcm_fifo_drained_ea,begin,memory_order_release);
    return NULL;
}
static void* restore_without_progress(void* unused)
{
    (void)unused;usleep(1000000);
    cellGcmSetHostPresentationSuspended(0);
    return NULL;
}
int main(void)
{
    setenv("PS3_STRICT_BOOT","1",1);
    ppu_hle_inject_base=0x20000;
    vm_base=mmap(NULL,ppu_vm_size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    CHECK(vm_base!=MAP_FAILED);
    memset(s_io_address_table,0xFF,sizeof(s_io_address_table));
    s_io_address_table[0]=0;s_io_address_table[1]=1;
    prepare();atomic_store(&g_gcm_fifo_drained_ea,begin);
    pthread_t reader;CHECK(!pthread_create(&reader,NULL,consume,NULL));
    cellGcm_fifo_recycle(ctx);CHECK(!pthread_join(reader,NULL));
    CHECK(vm_read32(ctx+8)==begin);
    CHECK(vm_read32(current)==(0x20000000u|begin));
    CHECK(vm_read32(GCM_CONTROL_GUEST_ADDR)==begin);
    prepare();cellGcmSetHostPresentationSuspended(1);
    CHECK(!pthread_create(&reader,NULL,restore_and_consume,NULL));
    cellGcm_fifo_recycle(ctx);CHECK(!pthread_join(reader,NULL));
    CHECK(vm_read32(ctx+8)==begin);
    prepare();pid_t child=fork();CHECK(child>=0);
    if(!child) { cellGcm_fifo_recycle(ctx);_Exit(99); }
    int status=0;CHECK(waitpid(child,&status,0)==child);
    CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==86);
    CHECK(vm_read32(ctx+8)==current); /* Failed drain never exposes unread storage. */
    prepare();child=fork();CHECK(child>=0);
    if(!child) {
        cellGcmSetHostPresentationSuspended(1);
        CHECK(!pthread_create(&reader,NULL,restore_without_progress,NULL));
        cellGcm_fifo_recycle(ctx);_Exit(99);
    }
    CHECK(waitpid(child,&status,0)==child);
    CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==86);
    CHECK(vm_read32(ctx+8)==current);
    munmap(vm_base,ppu_vm_size);
    puts("strict refill: delayed drain and six-second host suspension preserve unread commands; ordinary and resumed stalls exit86 without rewind");
    return 0;
}

/* Real syscall implementation and host threads; no GPU or game data. */
#include <assert.h>
#include <stdatomic.h>
#include "../runtime/syscalls/sys_ppu_thread.c"
uint8_t* vm_base;
uint32_t ppu_vm_size, ppu_hle_inject_base;
void ppu_resv_register(ppu_context* c) { (void)c; }
uint32_t vm_read32(uint64_t a) { uint32_t x=0;for(int i=0;i<4;i++)x=(x<<8)|vm_base[a+i];return x; }
int ppu_callback_dispatch_active(void) { return 0; }
void ppu_callback_worker_guest_exit(void) { abort(); }
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }
static atomic_int entered, cleaned, release_worker;
static uint64_t read_be64(uint32_t a) { uint64_t x=0;for(int i=0;i<8;i++)x=(x<<8)|vm_base[a+i];return x; }
static void entry(ppu_context* c) {
    atomic_fetch_add(&entered,1);
    while (!atomic_load(&release_worker)) {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
    c->gpr[3]=73;
    sys_ppu_thread_exit(c);
    abort();
}
static void cleanup(ppu_context* c) { (void)c; atomic_fetch_add(&cleaned,1); }
static void pause_host(void) {
#ifdef _WIN32
    Sleep(1);
#else
    usleep(1000);
#endif
}
static uint64_t create(ppu_context* c,int joinable) {
    c->gpr[3]=0x10000;c->gpr[4]=0x20000;c->gpr[5]=0;
    c->gpr[6]=100;c->gpr[7]=0x4000;c->gpr[8]=joinable;c->gpr[9]=0;
    assert(sys_ppu_thread_create(c)==CELL_OK);
    return read_be64(0x10000);
}
int main(void) {
    assert(vm_init()==CELL_OK); vm_stack_alloc_init(&g_vm_stack_alloc);
    ppu_context c; ppu_context_init(&c); c.thread_id=ppu_thread_register_main();
    g_ppu_thread_entry_trampoline=entry;g_ppu_thread_cleanup_trampoline=cleanup;
    atomic_store(&release_worker,1);
    for(int i=0;i<512;i++) {
        uint64_t tid=create(&c,0); assert(!g_ppu_threads[tid-1].joinable);
        while(atomic_load(&cleaned)<i+1)pause_host();
        /* Wait for final publication, not just cleanup entry. */
        for (;;) { table_lock();int done=g_ppu_threads[tid-1].state==PPU_THREAD_STATE_FINISHED;table_unlock();if(done)break;pause_host(); }
    }
    uint64_t tid=create(&c,1); c.gpr[3]=tid;c.gpr[4]=0x10008;
    assert(sys_ppu_thread_join(&c)==CELL_OK);assert(read_be64(0x10008)==73);
    atomic_store(&release_worker,0);tid=create(&c,1);
    while(atomic_load(&entered)<514)pause_host();
    c.gpr[3]=tid;assert(sys_ppu_thread_detach(&c)==CELL_OK);
    c.gpr[3]=tid;c.gpr[4]=0;assert(sys_ppu_thread_join(&c)==(int64_t)(int32_t)CELL_EINVAL);
    atomic_store(&release_worker,1);while(atomic_load(&cleaned)<514)pause_host();
    /* Creation must preserve an unjoined joinable completion. */
    uint64_t held=create(&c,1);while(atomic_load(&cleaned)<515)pause_host();
    tid=create(&c,0);assert(tid!=held);
    c.gpr[3]=held;c.gpr[4]=0;assert(sys_ppu_thread_join(&c)==CELL_OK);
    while(atomic_load(&cleaned)<516)pause_host();
    /* A genuinely full live table must still reject creation. */
    atomic_store(&release_worker,0);
    uint64_t live[PPU_THREAD_MAX-1];
    for(int i=0;i<PPU_THREAD_MAX-1;i++) {
        live[i]=create(&c,1);
        for(int j=0;j<i;j++)assert(live[i]!=live[j]);
    }
    assert(sys_ppu_thread_create(&c)==(int64_t)(int32_t)CELL_EAGAIN);
    atomic_store(&release_worker,1);
    for(int i=0;i<PPU_THREAD_MAX-1;i++) {
        c.gpr[3]=live[i];c.gpr[4]=0;assert(sys_ppu_thread_join(&c)==CELL_OK);
    }
    assert(atomic_load(&cleaned)==516+PPU_THREAD_MAX-1);
    printf("PPU lifetime: 512 detached creates, exit cleanup, join status, running detach and retained joinable passed\n");
    return 0;
}

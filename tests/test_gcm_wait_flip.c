/* CPU-only regression of cellGcmSetWaitFlip / cellGcmSetWaitFlipUnsafe and
 * the label-1 completion performed by cellGcmTickFlip. No GPU or game data.
 * Behaviour reference: RPCS3 cellGcmSys.cpp / sys_rsx.cpp (GPL-2.0). */
#include "../libs/video/cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define HAVE_CHILD 1
#endif

uint8_t* vm_base;
uint32_t ppu_vm_size=0x200000;
uint32_t ppu_hle_inject_base;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea;return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea,uint64_t value,int width)
{ (void)ea;(void)value;(void)width; }
/* NULL guest caller: TickFlip only records the pending handler event. */
ps3_guest_caller_fn g_ps3_guest_caller=NULL;

#define CTX 0x10000u
#define BEGIN 0x100000u
#define END 0x1FF000u
#define OPD 0x00012340u
#define CANARY 0xA5A5A5A5u
#define CTXP ((void*)(uintptr_t)CTX)

#define CHECK(x) do {if(!(x)){fprintf(stderr,"wait flip failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)

enum { CB_COUNT, CB_REFILL, CB_FAIL, CB_FORBIDDEN };
static int s_cb_mode;
static int s_cb_calls;
static u32 s_cb_opd,s_cb_ctx,s_cb_count;
static u32 s_refill_target;

static s32 test_callback(u32 opd,u32 context,u32 count)
{
    s_cb_calls++;s_cb_opd=opd;s_cb_ctx=context;s_cb_count=count;
    switch(s_cb_mode){
    case CB_REFILL: vm_write32(context+8,s_refill_target);return 0;
    case CB_FAIL: return (s32)0x80410001u;
#ifdef HAVE_CHILD
    case CB_FORBIDDEN: _exit(77);
#endif
    default: return 0;
    }
}

static void reset_env(u32 cur)
{
    memset(vm_base,0,ppu_vm_size);
    memset(s_io_address_table,0xFF,sizeof(s_io_address_table));
    memset(s_ea_address_table,0xFF,sizeof(s_ea_address_table));
    s_io_address_table[0]=0;s_io_address_table[1]=1;
    s_ea_address_table[0]=0;s_ea_address_table[1]=1;
    s_display_buffer_set[0]=1;
    vm_write32(CTX,BEGIN);vm_write32(CTX+4,END);
    vm_write32(CTX+8,cur);vm_write32(CTX+12,OPD);
    s_fifo_getoff=BEGIN+GCM_RECYCLE_MARGIN+4;
    vm_write32(GCM_CONTROL_GUEST_ADDR,0x10203040u);
    vm_write32(GCM_CONTROL_GUEST_ADDR+4,0x50607080u);
    s_cb_mode=CB_COUNT;s_cb_calls=0;s_cb_opd=s_cb_ctx=s_cb_count=0;
    s_refill_target=BEGIN+0x400;
    gcm_set_host_context_callback(test_callback);
}

static int packet_at(u32 a)
{
    static const uint8_t want[12]={0x00,0x08,0x00,0x64,0x00,0x00,0x00,0x10,0,0,0,0};
    return memcmp(vm_base+a,want,sizeof(want))==0;
}

static int words_are(u32 a,u32 v,int n)
{
    int i;for(i=0;i<n;i++) if(vm_read32(a+4u*(u32)i)!=v) return 0;
    return 1;
}

static int test_exact_packet(void)
{
    const u32 cur=BEGIN+0x2000;
    u32 get,st;
    reset_env(cur);
    vm_write32(cur-4,CANARY);vm_write32(cur+12,CANARY);
    cellGcmResetFlipStatus(); /* WAITING must not be consumed by WaitFlip */
    st=cellGcmGetFlipStatus();get=s_fifo_getoff;
    cellGcmSetWaitFlip(CTXP);
    CHECK(packet_at(cur));
    CHECK(vm_read32(cur)==0x00080064u && vm_read32(cur+4)==0x10u && vm_read32(cur+8)==0);
    CHECK(vm_read32(CTX+8)==cur+12);
    CHECK(vm_read32(cur-4)==CANARY && vm_read32(cur+12)==CANARY);
    CHECK(vm_read32(CTX)==BEGIN && vm_read32(CTX+4)==END && vm_read32(CTX+12)==OPD);
    CHECK(s_fifo_getoff==get);
    CHECK(vm_read32(GCM_CONTROL_GUEST_ADDR)==0x10203040u);
    CHECK(vm_read32(GCM_CONTROL_GUEST_ADDR+4)==0x50607080u);
    CHECK(cellGcmGetFlipStatus()==st);
    CHECK(s_cb_calls==0);
    cellGcmSetWaitFlipUnsafe(CTXP);
    CHECK(packet_at(cur+12) && vm_read32(CTX+8)==cur+24);
    CHECK(s_fifo_getoff==get && cellGcmGetFlipStatus()==st && s_cb_calls==0);
    return 0;
}

static int test_repeated(void)
{
    const u32 cur=BEGIN+0x100;
    u32 i;
    reset_env(cur);
    for(i=0;i<64;i++){
        if(i&1) cellGcmSetWaitFlipUnsafe(CTXP); else cellGcmSetWaitFlip(CTXP);
        CHECK(vm_read32(CTX+8)==cur+12*(i+1));
    }
    for(i=0;i<64;i++) CHECK(packet_at(cur+12*i));
    CHECK(vm_read32(cur+12*64)==0 && vm_read32(cur-4)==0);
    CHECK(s_cb_calls==0);
    return 0;
}

static int test_exact_twelve(void)
{
    reset_env(END-12);
    cellGcmSetWaitFlip(CTXP);
    CHECK(s_cb_calls==0 && packet_at(END-12) && vm_read32(CTX+8)==END);
    reset_env(END-12);
    cellGcmSetWaitFlipUnsafe(CTXP);
    CHECK(s_cb_calls==0 && packet_at(END-12) && vm_read32(CTX+8)==END);
    return 0;
}

static int test_shortage_refill(void)
{
    const u32 old=END-8;
    reset_env(old);
    vm_write32(old,CANARY);vm_write32(old+4,CANARY);
    s_cb_mode=CB_REFILL;s_refill_target=BEGIN+0x400;
    vm_write32(s_refill_target+12,CANARY);
    cellGcmSetWaitFlip(CTXP);
    CHECK(s_cb_calls==1);
    CHECK(s_cb_opd==OPD && s_cb_ctx==CTX && s_cb_count==8);
    CHECK(packet_at(s_refill_target));
    CHECK(vm_read32(CTX+8)==s_refill_target+12);
    CHECK(vm_read32(s_refill_target+12)==CANARY);
    CHECK(vm_read32(old)==CANARY && vm_read32(old+4)==CANARY);
    CHECK(vm_read32(CTX)==BEGIN && vm_read32(CTX+4)==END);
    return 0;
}

static int test_callback_error(void)
{
    const u32 old=END-4;
    u32 st;
    reset_env(old);
    vm_write32(old,CANARY);
    cellGcmResetFlipStatus();st=cellGcmGetFlipStatus();
    s_cb_mode=CB_FAIL;
    cellGcmSetWaitFlip(CTXP);
    CHECK(s_cb_calls==1 && s_cb_count==8 && s_cb_ctx==CTX && s_cb_opd==OPD);
    CHECK(vm_read32(CTX+8)==old && vm_read32(old)==CANARY);
    CHECK(words_are(BEGIN,0,64));
    CHECK(cellGcmGetFlipStatus()==st);
    return 0;
}

static int test_completion(void)
{
    const u32 cur=BEGIN+0x800;
    u32 base,i;
    reset_env(cur);
    base=(u32)(uintptr_t)cellGcmGetLabelAddress(1)-0x10u;
    CHECK(base!=0 && base+0x30<=ppu_vm_size);
    for(i=0;i<4;i++){
        vm_write32(base+4*i,0x11111111u);
        vm_write32(base+0x10+4*i,0x22222222u);
        vm_write32(base+0x20+4*i,0x33333333u);
    }
    cellGcmSetWaitFlip(CTXP);
    CHECK(packet_at(cur) && vm_read32(CTX+8)==cur+12);
    CHECK(words_are(base+0x10,0x22222222u,4)); /* WaitFlip itself never completes */
    cellGcmTickFlip();
    CHECK(words_are(base+0x10,0,4));
    CHECK(words_are(base,0x11111111u,4));
    CHECK(words_are(base+0x20,0x33333333u,4));
    /* Completion does not enqueue or present: FIFO and cursor untouched. */
    CHECK(packet_at(cur) && vm_read32(CTX+8)==cur+12);
    CHECK(words_are(cur+12,0,64));
    CHECK(s_cb_calls==0);
    return 0;
}

/* Flip handler delivery: one call per completed requested flip. */
#define FLIP_OPD 0x00012400u
static int s_flip_calls;
static void count_flip(uint32_t opd,uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3,
                       uint64_t a4,uint64_t a5,uint64_t a6,uint64_t a7)
{
    (void)a0;(void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;
    if(opd==FLIP_OPD) s_flip_calls++;
}

static void reset_flip_handler(void)
{
    reset_env(BEGIN+0x800);
    GCM_FLIP_COUNT_RESET(s_flip_outstanding);
    GCM_FLIP_COUNT_RESET(s_flip_completions);
    (void)GCM_PENDING_TAKE();
    s_flip_status=CELL_GCM_FLIP_STATUS_DONE;
    s_flip_handler_opd=FLIP_OPD;s_flip_handler_code=0;
    s_flip_calls=0;
    g_ps3_guest_caller=count_flip;
}

static int finish_flip_handler(int r)
{
    g_ps3_guest_caller=NULL;s_flip_handler_opd=0;
    GCM_FLIP_COUNT_RESET(s_flip_outstanding);
    GCM_FLIP_COUNT_RESET(s_flip_completions);
    return r;
}

/* Folklore: flip N+1 is requested before N completes. Both completions must
 * reach the handler; the second used to see DONE and was dropped. */
static int test_two_flips_in_flight(void)
{
    reset_flip_handler();
    CHECK(gcm_flip_request(0,1)==CELL_OK);
    CHECK(gcm_flip_request(0,1)==CELL_OK);
    CHECK(s_flip_calls==0);                 /* strict: nothing at request time */
    cellGcmTickFlip();ppu_gcm_pump();
    CHECK(s_flip_calls==1 && cellGcmGetFlipStatus()==CELL_GCM_FLIP_STATUS_DONE);
    cellGcmTickFlip();ppu_gcm_pump();
    CHECK(s_flip_calls==2);
    cellGcmTickFlip();ppu_gcm_pump();       /* nothing left outstanding */
    CHECK(s_flip_calls==2);
    return finish_flip_handler(0);
}

/* Two completions before anyone pumps: two handler calls, not one. */
static int test_completions_not_coalesced(void)
{
    reset_flip_handler();
    CHECK(gcm_flip_request(0,1)==CELL_OK && gcm_flip_request(0,1)==CELL_OK);
    cellGcmTickFlip();cellGcmTickFlip();
    ppu_gcm_pump();
    CHECK(s_flip_calls==2);
    ppu_gcm_pump();
    CHECK(s_flip_calls==2);
    return finish_flip_handler(0);
}

/* No flip requested, no callback -- even with the status set to WAITING by
 * cellGcmResetFlipStatus (Tokyo Jungle's poll loop relies on this). */
static int test_no_request_no_handler(void)
{
    reset_flip_handler();
    cellGcmResetFlipStatus();
    cellGcmTickFlip();ppu_gcm_pump();
    CHECK(s_flip_calls==0 && cellGcmGetFlipStatus()==CELL_GCM_FLIP_STATUS_WAITING);
    return finish_flip_handler(0);
}

/* The legacy vblank-ticker beat (cellGcm_request_tick) still delivers once. */
static int test_legacy_tick_delivers_once(void)
{
    reset_flip_handler();
    cellGcm_request_tick();cellGcm_request_tick();
    ppu_gcm_pump();
    CHECK(s_flip_calls==1);
    return finish_flip_handler(0);
}

#ifdef HAVE_CHILD
static void child_misaligned(void) { reset_env(BEGIN+2);cellGcmSetWaitFlip(CTXP); }
static void child_short_unsafe(void)
{ reset_env(END-8);s_cb_mode=CB_FORBIDDEN;cellGcmSetWaitFlipUnsafe(CTXP); }
static void child_missing_callback(void)
{ reset_env(END-8);gcm_set_host_context_callback(NULL);cellGcmSetWaitFlip(CTXP); }
static void child_no_space(void) { reset_env(END-8);s_cb_mode=CB_COUNT;cellGcmSetWaitFlip(CTXP); }
static void child_unmapped(void)
{ reset_env(BEGIN);s_io_address_table[1]=0xFFFF;cellGcmSetWaitFlip(CTXP); }

static int expect_strict_fatal(void (*fn)(void))
{
    pid_t p;int st=0;
    fflush(stdout);fflush(stderr);
    p=fork();
    CHECK(p>=0);
    if(p==0){fn();_exit(0);}
    CHECK(waitpid(p,&st,0)==p);
    if(!WIFEXITED(st)||WEXITSTATUS(st)!=86){
        fprintf(stderr,"child status 0x%x, expected exit 86\n",st);return 1;
    }
    return 0;
}
static int test_fatal_misaligned(void) { return expect_strict_fatal(child_misaligned); }
static int test_fatal_short_unsafe(void) { return expect_strict_fatal(child_short_unsafe); }
static int test_fatal_missing_callback(void) { return expect_strict_fatal(child_missing_callback); }
static int test_fatal_no_space(void) { return expect_strict_fatal(child_no_space); }
static int test_fatal_unmapped(void) { return expect_strict_fatal(child_unmapped); }
#endif

struct test_case { const char* name; int (*fn)(void); };

int main(void)
{
    static const struct test_case cases[]={
        {"exact packet, cursor, canaries, put/get/status unchanged",test_exact_packet},
        {"repeated calls use distinct positions",test_repeated},
        {"exact 12 bytes remaining needs no callback",test_exact_twelve},
        {"shortage invokes callback once and writes in new space",test_shortage_refill},
        {"callback error leaves command unwritten",test_callback_error},
        {"tick completion clears only label 1",test_completion},
        {"two flips in flight both reach the handler",test_two_flips_in_flight},
        {"completions before one pump are not coalesced",test_completions_not_coalesced},
        {"no requested flip, no handler",test_no_request_no_handler},
        {"legacy vblank tick delivers once",test_legacy_tick_delivers_once},
#ifdef HAVE_CHILD
        {"strict fatal: misaligned current",test_fatal_misaligned},
        {"strict fatal: short unsafe, no callback",test_fatal_short_unsafe},
        {"strict fatal: missing callback",test_fatal_missing_callback},
        {"strict fatal: callback returns 0 without space",test_fatal_no_space},
        {"strict fatal: unmapped command words",test_fatal_unmapped},
#endif
    };
    size_t i,n=sizeof(cases)/sizeof(cases[0]);
    int failed=0;
#ifdef _WIN32
    _putenv_s("PS3_STRICT_BOOT","1");
#else
    setenv("PS3_STRICT_BOOT","1",1);
#endif
    ppu_hle_inject_base=0x20000;
    vm_base=(uint8_t*)calloc(1,ppu_vm_size);
    if(!vm_base){fprintf(stderr,"wait flip: no memory\n");return 1;}
    for(i=0;i<n;i++){
        int r=cases[i].fn();
        printf("%s: %s\n",r?"FAIL":"PASS",cases[i].name);
        failed+=r!=0;
    }
    free(vm_base);
    printf("wait flip: %d/%d passed\n",(int)n-failed,(int)n);
    return failed?1:0;
}

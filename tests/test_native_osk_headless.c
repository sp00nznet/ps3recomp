/* Headless native OSK host: LoadAsync without Init, real 0x502/0x503/0x504
 * events, confirmation only through the control mailbox, initial text returned. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../libs/misc/cellOskDialog.h"
#include "../libs/system/cellSysutil.h"
#include "ps3emu/guest_call.h"
#include "../libs/misc/osk_native.h"
uint8_t memory[65536]; uint8_t* vm_base=memory; uint32_t ppu_vm_size=sizeof(memory);
uint32_t g_ww_lo,g_ww_hi; int g_resv_store_active;
void ppu_resv_break_store(uint64_t a){(void)a;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int z){(void)a;(void)v;(void)z;}
void vm_write32(uint64_t a,uint32_t v){assert(a+4<=sizeof(memory));v=__builtin_bswap32(v);memcpy(memory+a,&v,4);}
void vm_write8(uint64_t a,uint8_t v){assert(a<sizeof(memory));memory[a]=v;}
int spu_coh_is_reserved(uint32_t a){(void)a;return 0;}
void spu_lockline_lock(void){}
void spu_lockline_unlock(void){}
void spu_coh_notify_write(uint32_t a){(void)a;}
void cellMsgDialog_tick(void){}
void np_psnr_pump(void){}
void ps3_np_eula_tick(void){}
static uint32_t statuses[16]; static unsigned events;
static void cb(u32 opd,u64 status,u64 param,u64 user,u64 d,u64 e,u64 f,u64 g,u64 h){
 (void)opd;(void)param;(void)user;(void)d;(void)e;(void)f;(void)g;(void)h;statuses[events++]=(uint32_t)status;
}
static void put16(uint32_t a,uint16_t v){v=__builtin_bswap16(v);memcpy(memory+a,&v,2);}
static uint32_t read32(uint32_t a){uint32_t x;memcpy(&x,memory+a,4);return __builtin_bswap32(x);}
static uint16_t read16(uint32_t a){uint16_t x;memcpy(&x,memory+a,2);return __builtin_bswap16(x);}
static void command(const char* path,const char* text){FILE* f=fopen(path,"w");assert(f);fputs(text,f);assert(!fclose(f));}
int main(void){
 const char* path=getenv("OSK_NATIVE_CONTROL_FILE");assert(path && osk_native_available());
 memset(memory,0,sizeof(memory));
 vm_write32(0x1000,0x2000);vm_write32(0x1004,0x2100);vm_write32(0x1008,32); /* message, initial, limit */
 put16(0x2000,'N');put16(0x2100,'x');
 g_ps3_guest_caller=cb;assert(!cellSysutilRegisterCallback(1,(void*)0x3000,(void*)0xDEAD));
 assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));          /* no artificial Init */
 cellSysutilCheckCallback();assert(events==1 && statuses[0]==0x502);
 for(int i=0;i<5;i++)cellSysutilCheckCallback();assert(events==1);        /* nothing confirms it by itself */
 command(path,"confirm 2\n");cellSysutilCheckCallback();assert(events==1);  /* wrong generation */
 command(path,"confirm 1\n");cellSysutilCheckCallback();cellSysutilCheckCallback();
 assert(events==2 && statuses[1]==0x503 && access(path,F_OK));
 vm_write32(0x1200,0xA5A5A5A5);vm_write32(0x1204,32);vm_write32(0x1208,0x2200);
 assert(!cellOskDialogUnloadAsync((void*)0x1200));
 assert(read32(0x1200)==0);
 const char* want="PlayerHost";
 for(unsigned i=0;i<strlen(want);i++)assert(read16(0x2200+2*i)==(uint16_t)want[i]);
 assert(read16(0x2200+2*strlen(want))==0);
 cellSysutilCheckCallback();assert(events==3 && statuses[2]==0x504);
 /* A second dialog can be aborted without the mailbox. */
 assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));cellSysutilCheckCallback();
 assert(!cellOskDialogAbort());cellSysutilCheckCallback();assert(statuses[events-1]==0x503);
 assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(read32(0x1200)==2);
 puts("headless native OSK: lifecycle, mailbox-only confirm, initial text, abort");
 return 0;
}

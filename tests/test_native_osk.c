#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
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
int spu_coh_is_reserved(uint32_t a){return 0;}
void spu_lockline_lock(void){}
void spu_lockline_unlock(void){}
void spu_coh_notify_write(uint32_t a){}
void cellMsgDialog_tick(void){}
void np_psnr_pump(void){}
void ps3_np_eula_tick(void){}
static void put16(uint32_t a,uint16_t v){v=__builtin_bswap16(v);memcpy(memory+a,&v,2);}
static OskNativeRequest request; static unsigned opens,closes,events;static uint32_t statuses[100];
static int available=1;
static unsigned confirms;static uint64_t confirmed_token;
void osk_native_confirm(uint64_t token){confirms++;confirmed_token=token;}
int osk_native_available(void){return available;}
void osk_native_open(const OskNativeRequest* r){request=*r;opens++;}
void osk_native_close(uint64_t token){assert(token==request.token);closes++;}
static void cb(u32 opd,u64 status,u64 param,u64 user,u64 d,u64 e,u64 f,u64 g,u64 h){
 assert(opd==0x3000 && user==0xDEAD && !param && !d && !e && !f && !g && !h);
 statuses[events++]=(uint32_t)status;
}
static uint32_t read32(uint32_t a){uint32_t x;memcpy(&x,memory+a,4);return __builtin_bswap32(x);}
static uint16_t read16(uint32_t a){uint16_t x;memcpy(&x,memory+a,2);return __builtin_bswap16(x);}
static int32_t load(void){return cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000);}
static void result_at(uint32_t capacity){vm_write32(0x1200,0xA5A5A5A5);vm_write32(0x1204,capacity);vm_write32(0x1208,0x2200);}
static void* background_finish(void* text){osk_native_did_finish(request.token,0,text,4);return NULL;}
static void guest_name(void){
 const uint16_t name[]={'P','l','a','y','e','r',0};
 for(unsigned i=0;i<sizeof(name)/sizeof(name[0]);i++)put16(0x2100+i*2,name[i]);
}
static void assert_initial(const uint16_t* want){
 unsigned i=0;do {assert(request.initial[i]==want[i]);}while(want[i++]);
}
static void abort_and_unload(void){
 assert(!cellOskDialogAbort());cellSysutilCheckCallback();result_at(32);
 assert(!cellOskDialogUnloadAsync((void*)0x1200));cellSysutilCheckCallback();
}
/* Request/mailbox boundary test: an explicit host prefill must not submit text,
 * change the guest default, affect secure fields, or overwrite later input. */
static int initial_text_case(const char* mode){
 const uint16_t original[]={'P','l','a','y','e','r',0};
 const uint16_t ascii[]={'P','l','a','y','e','r','H','o','s','t',0};
 const uint16_t unicode[]={'A',0xA2,0x03A9,0x20AC,0xD83D,0xDE80,0};
 const uint16_t pair[]={'A',0xD83D,0xDE80,0},prefix[]={'A',0},empty[]={0};
 const uint16_t edited[]={'E','d','i','t',0};
 const uint16_t* want=ascii;uint32_t limit=32;uint16_t maximum[512]={0};
 if(!strcmp(mode,"unicode"))want=unicode;
 else if(!strcmp(mode,"pair")){want=pair;limit=3;}
 else if(!strcmp(mode,"split")){want=prefix;limit=2;}
 else if(!strcmp(mode,"zero")){want=empty;limit=0;}
 else if(!strcmp(mode,"empty"))want=empty;
 else if(!strcmp(mode,"unchanged"))want=original;
 else if(!strcmp(mode,"maximum")){
  for(unsigned i=0;i<510;i++)maximum[i]='a';want=maximum;limit=512;
 }
 else assert(!strcmp(mode,"ascii"));
 memset(memory,0,sizeof(memory));
 vm_write32(0x1000,0x2000);vm_write32(0x1004,0x2100);vm_write32(0x1008,32);
 put16(0x2000,'N');guest_name();
 vm_write32(0x1100,0x20000000);assert(!load());assert_initial(original);
 vm_write32(0x1100,0);assert(load()==(int32_t)0x8002B105); // busy cannot consume the option
 abort_and_unload();
 vm_write32(0x1008,513);assert(load()==(int32_t)0x8002B504);
 vm_write32(0x1008,limit);available=0;assert(load()==(int32_t)0x8002B503);available=1;
 assert(!load());assert_initial(want);
 for(unsigned i=0;i<7;i++)assert(read16(0x2100+i*2)==original[i]); // copied host default only
 osk_native_did_open(request.token);cellSysutilCheckCallback();
 for(unsigned i=0;i<10;i++)cellSysutilCheckCallback();
 result_at(32);assert(!cellOskDialogGetInputText((void*)0x1200));assert(read32(0x1200)==3);
 assert(cellOskDialogUnloadAsync((void*)0x1200)==(int32_t)0x8002B105); // still needs confirmation
 osk_native_did_finish(request.token,1,NULL,0);cellSysutilCheckCallback();
 assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(read32(0x1200)==1);cellSysutilCheckCallback();
 assert(!setenv("PS3RECOMP_OSK_INITIAL_TEXT","Late replacement",1));
 vm_write32(0x1008,32);assert(!load());assert_initial(original);
 osk_native_did_open(request.token);cellSysutilCheckCallback();
 osk_native_did_finish(request.token,0,edited,4);cellSysutilCheckCallback();result_at(32);
 assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(read32(0x1200)==0);
 for(unsigned i=0;i<5;i++)assert(read16(0x2200+i*2)==edited[i]);cellSysutilCheckCallback();
 printf("Native OSK initial text case %s passed\n",mode);return 0;
}

/* Rejecting pre-open, stale, oversized, and repeated commands is observable at
 * the actual host boundary; the adapter itself never completes guest input. */
static void command(const char* text){
 const char* path=getenv("TEST_CONTROL_FILE");assert(path);
 FILE* f=fopen(path,"w");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));
}
static void confirm_command(uint64_t token){char line[64];snprintf(line,sizeof(line),"confirm %llu\n",(unsigned long long)token);command(line);}
static int control_case(int enabled){
 const char* path=getenv("TEST_CONTROL_FILE");assert(path);
 memset(memory,0,sizeof(memory));vm_write32(0x1000,0x2000);vm_write32(0x1004,0x2100);vm_write32(0x1008,32);
 put16(0x2000,'N');guest_name();assert(!load());uint64_t token=request.token;
 confirm_command(token);cellOskDialog_tick();assert(!confirms);
 if(!enabled){
  assert(!access(path,F_OK));assert(!setenv("OSK_NATIVE_CONTROL_FILE",path,1));
  osk_native_did_open(token);cellOskDialog_tick();assert(!confirms && !access(path,F_OK)); // opt-in is cached at startup
  assert(!cellOskDialogAbort());cellOskDialog_tick();result_at(32);assert(!cellOskDialogUnloadAsync((void*)0x1200));cellOskDialog_tick();
  puts("Unconfigured native OSK control leaves file and lifecycle untouched");return 0;
 }
 assert(access(path,F_OK)); // consumed during LOADING; must not later submit
 osk_native_did_open(token);cellOskDialog_tick();assert(!confirms);
 const char* invalid[]={"", "confirm", "confirm 0\n", "confirm -1\n", "confirm +1\n", "confirm 1 junk\n", "confirm 18446744073709551616\n", "confirm 1\nconfirm 1\n", "bogus 1\n"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){
  command(invalid[i]);cellOskDialog_tick();assert(!confirms && access(path,F_OK));
 }
 FILE* binary=fopen(path,"w");assert(binary);const char embedded[]="confirm 1\0hidden";
 assert(fwrite(embedded,1,sizeof(embedded)-1,binary)==sizeof(embedded)-1);assert(!fclose(binary));
 cellOskDialog_tick();assert(!confirms && access(path,F_OK));
 char oversized[200];memset(oversized,'x',sizeof(oversized)-1);oversized[sizeof(oversized)-1]=0;
 command(oversized);cellOskDialog_tick();assert(!confirms && access(path,F_OK));
 confirm_command(token+1);cellOskDialog_tick();assert(!confirms && access(path,F_OK));
 confirm_command(token);cellOskDialog_tick();assert(confirms==1 && confirmed_token==token && access(path,F_OK));
 result_at(32);assert(!cellOskDialogGetInputText((void*)0x1200));assert(read32(0x1200)==3);
 assert(cellOskDialogUnloadAsync((void*)0x1200)==(int32_t)0x8002B105);
 confirm_command(token);cellOskDialog_tick();assert(confirms==1 && access(path,F_OK));
 assert(!cellOskDialogAbort());confirm_command(token);cellOskDialog_tick();assert(confirms==1 && access(path,F_OK));
 result_at(32);assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(read32(0x1200)==2);cellOskDialog_tick();
 assert(!load());osk_native_did_open(request.token);confirm_command(token);cellOskDialog_tick();assert(confirms==1);
 confirm_command(request.token);cellOskDialog_tick();assert(confirms==2 && confirmed_token==request.token);
 assert(!cellOskDialogAbort());cellOskDialog_tick();result_at(32);assert(!cellOskDialogUnloadAsync((void*)0x1200));cellOskDialog_tick();
 puts("Native OSK control consumption / token / open race / cancellation / one-shot checks passed");return 0;
}

int main(int argc,char** argv){
 assert(!osk_native_has_ui_result(0));
 if(argc==2 && !strcmp(argv[1],"control"))return control_case(1);
 if(argc==2 && !strcmp(argv[1],"control-off"))return control_case(0);
 if(argc==2)return initial_text_case(argv[1]);
 memset(memory,0xA5,sizeof(memory));
 vm_write32(0x1000,0x2000);vm_write32(0x1004,0x2100);vm_write32(0x1008,32);
 put16(0x2000,'N');put16(0x2002,0);put16(0x2100,0);
 memset(memory+0x1100,0,20);
 int32_t rc=cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000);
 if(rc){fprintf(stderr,"FAIL: valid LoadAsync without cellOskDialogInit returned %08X\n",(uint32_t)rc);return 1;}
 assert(opens==1 && request.message[0]=='N' && request.limit==32);
 assert(load()==(int32_t)0x8002B105);
 g_ps3_guest_caller=cb;assert(!cellSysutilRegisterCallback(1,(void*)0x3000,(void*)0xDEAD));
 cellSysutilCheckCallback();assert(events==0); // no fabricated loaded or completion
 osk_native_did_open(request.token);assert(events==0);assert(!osk_native_has_ui_result(request.token));
 cellSysutilCheckCallback();assert(events==1 && statuses[0]==0x502);
 for(int i=0;i<10;i++)cellSysutilCheckCallback();assert(events==1);
 uint16_t text[]={'P',0x03A9,0xD83D,0xDE80,0};
 pthread_t worker;assert(!pthread_create(&worker,NULL,background_finish,text));assert(!pthread_join(worker,NULL));assert(events==1);
 cellSysutilCheckCallback();assert(events==2 && statuses[1]==0x503);assert(osk_native_has_ui_result(request.token));
 result_at(2);assert(!cellOskDialogUnloadAsync((void*)0x1200));
 assert(read32(0x1200)==0 && read32(0x1204)==2);
 assert(read16(0x2200)=='P' && read16(0x2202)==0x03A9 && read16(0x2204)==0 && memory[0x2206]==0xA5);
 assert(events==2);cellSysutilCheckCallback();assert(events==3 && statuses[2]==0x504);
 assert(!load());uint64_t token=request.token;assert(!osk_native_has_ui_result(token-1) && !osk_native_has_ui_result(token));
 osk_native_did_open(token-1);cellSysutilCheckCallback();assert(events==3);
 osk_native_did_open(token);cellSysutilCheckCallback();assert(statuses[events-1]==0x502);
 assert(cellOskDialogUnloadAsync((void*)0x1200)==(int32_t)0x8002B105);
 osk_native_did_finish(token,1,NULL,0);cellSysutilCheckCallback();assert(osk_native_has_ui_result(token));
 result_at(32);memory[0x2200]=0xA5;assert(!cellOskDialogUnloadAsync((void*)0x1200));
 assert(read32(0x1200)==1 && memory[0x2200]==0xA5);cellSysutilCheckCallback();
 assert(!load());token=request.token;assert(!cellOskDialogAbort());
 osk_native_did_open(token);osk_native_did_finish(token,0,text,4);
 cellSysutilCheckCallback();assert(statuses[events-1]==0x503);assert(!osk_native_has_ui_result(token));
 result_at(32);assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(read32(0x1200)==2);
 cellSysutilCheckCallback();
 available=0;assert(load()==(int32_t)0x8002B503);available=1;
 vm_write32(0x1008,513);assert(load()==(int32_t)0x8002B504);vm_write32(0x1008,32);
 vm_write32(0x1004,0xFFFFFFFF);assert(load()==(int32_t)0x8002B504);vm_write32(0x1004,0x2100);
 assert(!load());osk_native_did_open(request.token);cellSysutilCheckCallback();
 osk_native_did_finish(request.token,0,text,4);cellSysutilCheckCallback();
 result_at(3);assert(!cellOskDialogUnloadAsync((void*)0x1200)); // never split surrogate
 assert(read16(0x2204)==0);cellSysutilCheckCallback();
 assert(!load());osk_native_did_open(request.token);cellSysutilCheckCallback();
 osk_native_did_finish(request.token,0,text,4);cellSysutilCheckCallback();
 result_at(32);vm_write32(0x1208,65534);assert(cellOskDialogUnloadAsync((void*)0x1200)==(int32_t)0x8002B504);
 assert(read32(0x1200)==0xA5A5A5A5);vm_write32(0x1208,0x2200);
 assert(!cellOskDialogUnloadAsync((void*)0x1200));cellSysutilCheckCallback();
 vm_write32(0x1008,1);put16(0x2100,0xD83D);put16(0x2102,0xDE80);put16(0x2104,0);
 assert(!load());assert(request.initial[0]==0); // initial text must not split a surrogate pair
 assert(!cellOskDialogAbort());cellSysutilCheckCallback();result_at(32);assert(!cellOskDialogUnloadAsync((void*)0x1200));cellSysutilCheckCallback();
 puts("Native OSK load / real sysutil lifecycle / Unicode / cancellation / stale token / bounded guest result checks passed");return 0;
}

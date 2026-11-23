/* Windows native OSK host over a real Win32 parent window: real NID dispatch
 * into cellOskDialog/cellSysutil, the real dialog controls, and the backends'
 * own message pumps. Built against a runtime with PS3RECOMP_NATIVE_OSK_WINDOWS
 * by tools/test_native_osk_windows.py. No game files. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "ppu_context.h"
#include "ps3emu/guest_call.h"
#include "../libs/misc/osk_native.h"
#include "../runtime/ppu/ppu_memory.h"
#include "../libs/video/rsx_null_backend.h"
#include "../libs/video/rsx_d3d12_backend.h"
uint8_t* vm_base;
struct fixture_entry {uint64_t addr;void* function;const char* name;};
const struct fixture_entry function_table[1]={{0,NULL,NULL}};
const uint64_t function_table_count=0;
extern void ppu_hle_register_all(void);
extern int ps3_hle_has(uint32_t);
extern void ps3_hle_call(uint32_t,ppu_context*);
static unsigned events;
static int legacy_pump;
static u64 loaded_generation;
static u32 statuses[64];static int inside_callback_pump;static unsigned heartbeat,checks;
#define CHECK(value) do{checks++;if(!(value)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#value);ExitProcess(43);}}while(0)
static void callback(u32 opd,u64 status,u64 param,u64 user,u64 a,u64 b,u64 c,u64 d,u64 e){
    CHECK(inside_callback_pump && opd==0x3000 && !param && user==0xCAFE && !a && !b && !c && !d && !e);
    CHECK(events<64);statuses[events++]=(u32)status;
}
static u32 call(u32 nid,u64 a,u64 b,u64 c){
    ppu_context context={0};context.gpr[1]=0x8000;context.gpr[3]=a;context.gpr[4]=b;context.gpr[5]=c;
    ps3_hle_call(nid,&context);if(nid==0x7FCFC915 && !(u32)context.gpr[3])loaded_generation++;
    return (u32)context.gpr[3];
}
static LRESULT CALLBACK parent_proc(HWND window,UINT message,WPARAM w,LPARAM l){return DefWindowProcW(window,message,w,l);}
#ifdef OSK_NATIVE_HAS_WIN32_HOST
static void pump_host(void){
    CHECK((legacy_pump?rsx_d3d12_backend_pump_messages():rsx_null_backend_pump_messages())==0);
    heartbeat++;
}
static void pump_guest(void){inside_callback_pump=1;CHECK(!call(0x189A74DA,0,0,0));inside_callback_pump=0;}
static HWND dialog(void){return FindWindowW(L"ps3recomp_osk",NULL);}
static DWORD WINAPI worker_load(void* unused){(void)unused;u32 result=call(0x7FCFC915,UINT64_MAX,0x1000,0x1200);osk_native_win32_pump();return result;}
static void command_file(const char* path,u64 token){FILE* file=fopen(path,"wb");CHECK(file!=NULL);CHECK(fprintf(file,"confirm %llu\n",(unsigned long long)token)>0);CHECK(!fclose(file));}
static void result_buffer(u32 capacity){vm_write32(0x1300,0xA5A5A5A5);vm_write32(0x1304,capacity);vm_write32(0x1308,0x2200);memset(vm_base+0x21FE,0xA5,1030);}
static void unload(u32 result,u32 capacity){
    unsigned count=events;result_buffer(capacity);CHECK(!call(0x3D1E1931,0x1300,0,0));
    CHECK(vm_read32(0x1300)==result && vm_read32(0x1304)==capacity && events==count);
    pump_host();CHECK(!osk_native_win32_input_active());pump_guest();CHECK(statuses[events-1]==0x504);
}
static void opened(void){
    unsigned count=events;pump_host();CHECK(dialog()!=NULL && events==count);
    pump_guest();CHECK(events==count+1 && statuses[count]==0x502);
    CHECK(osk_native_win32_input_active());
}
static void click(int id){HWND button=GetDlgItem(dialog(),id);CHECK(button!=NULL);SendMessageW(button,BM_CLICK,0,0);}
#endif
int main(int argc,char**argv){
    legacy_pump=argc>1 && !strcmp(argv[1],"legacy-pump");
    static uint8_t memory[65536];vm_base=memory;
    extern uint32_t ppu_vm_size;ppu_vm_size=sizeof memory;
    WNDCLASSW cls={0};cls.lpfnWndProc=parent_proc;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"OskSyntheticParent";
    RegisterClassW(&cls);
    HWND parent=CreateWindowExW(0,cls.lpszClassName,L"Synthetic OSK",WS_OVERLAPPEDWINDOW,0,0,640,480,NULL,NULL,cls.hInstance,NULL);
    if(!parent)return 2;
    ppu_hle_register_all();g_ps3_guest_caller=callback;
    vm_write32(0x1000,0);vm_write32(0x1010,0);
    vm_write32(0x1200,0x2000);vm_write32(0x1204,0x2100);vm_write32(0x1208,16);
    vm_write16(0x2000,'N');vm_write16(0x2002,0);vm_write16(0x2100,'P');vm_write16(0x2102,0);
    if(!ps3_hle_has(0x7FCFC915)||call(0x9D98AFA0,0,0x3000,0xCAFE))return 3;
#ifdef OSK_NATIVE_HAS_WIN32_HOST
    CHECK(call(0x7FCFC915,UINT64_MAX,0x1000,0x1200)==0x8002B503u);
    CHECK(call(0x7FCFC915,UINT64_MAX,0xFFF8,0x1200)==0x8002B504u);
    osk_native_win32_set_window(parent);
    vm_write32(0x1208,513);CHECK(call(0x7FCFC915,UINT64_MAX,0x1000,0x1200)==0x8002B504u);vm_write32(0x1208,16);
#endif
    if(argc>2 && !strcmp(argv[1],"control"))command_file(argv[2],1);
    const u32 result=call(0x7FCFC915,UINT64_MAX,0x1000,0x1200);
    if(result){fprintf(stderr,"FAIL direct LoadAsync without legacy Init returned %08X\n",result);return 41;}
    if(events){fprintf(stderr,"FAIL load delivered completion before UI/pump\n");return 42;}
#ifdef OSK_NATIVE_HAS_WIN32_HOST
    opened();CHECK(GetWindow(dialog(),GW_OWNER)==parent);
    WCHAR text[513]={0};GetWindowTextW(GetDlgItem(dialog(),1001),text,513);
    const WCHAR* want=argc>1 && !strcmp(argv[1],"prefill")?L"PlayerHost":argc>1 && !strcmp(argv[1],"unicode")?L"A\u03A9\U0001F680":L"P";
    CHECK(!wcscmp(text,want));
    HWND edit=GetDlgItem(dialog(),1001);SetWindowTextW(edit,L"");
    CHECK(PostMessageW(edit,WM_CHAR,0x3A9,1));CHECK(PostMessageW(edit,WM_CHAR,0xD83D,1));CHECK(PostMessageW(edit,WM_CHAR,0xDE80,1));
    pump_host();GetWindowTextW(edit,text,513);CHECK(!wcscmp(text,L"\u03A9\U0001F680"));
    for(unsigned i=0;i<20;i++)pump_host();CHECK(heartbeat>=21 && events==1);
    CHECK(call(0x7FCFC915,UINT64_MAX,0x1000,0x1200)==0x8002B105u);
    result_buffer(4);CHECK(call(0x3D1E1931,0x1300,0,0)==0x8002B105u);CHECK(vm_read32(0x1300)==0xA5A5A5A5);
    SetWindowTextW(GetDlgItem(dialog(),1001),L"A\u03A9\U0001F680");
    unsigned count=events;click(IDOK);CHECK(events==count && vm_read32(0x1300)==0xA5A5A5A5);
    pump_guest();CHECK(statuses[events-1]==0x503);
    unload(0,4);CHECK(vm_read16(0x21FE)==0xA5A5 && vm_read16(0x220A)==0xA5A5);
    CHECK(vm_read16(0x2200)=='A' && vm_read16(0x2202)==0x3A9 && vm_read16(0x2204)==0xD83D && vm_read16(0x2206)==0xDE80 && !vm_read16(0x2208));
    /* Secure fields retain the guest initial value and require real Cancel. */
    vm_write32(0x1000,0x20000000);CHECK(!call(0x7FCFC915,UINT64_MAX,0x1000,0x1200));opened();
    CHECK(GetWindowLongPtrW(GetDlgItem(dialog(),1001),GWL_STYLE)&ES_PASSWORD);
    GetWindowTextW(GetDlgItem(dialog(),1001),text,513);CHECK(!wcscmp(text,L"P"));
    click(IDCANCEL);pump_guest();CHECK(statuses[events-1]==0x503);unload(1,4);CHECK(vm_read16(0x2200)==0xA5A5);
    /* Abort before UI creation cannot open or complete a later generation. */
    vm_write32(0x1000,0);CHECK(!call(0x7FCFC915,UINT64_MAX,0x1000,0x1200));
    CHECK(!call(0xB6D84526,0,0,0));pump_host();CHECK(!dialog() && !osk_native_win32_input_active());
    pump_guest();CHECK(statuses[events-1]==0x503);unload(2,4);
    /* A result capacity cannot split a surrogate pair or overrun canaries. */
    vm_write32(0x1208,3);CHECK(!call(0x7FCFC915,UINT64_MAX,0x1000,0x1200));opened();
    SetWindowTextW(GetDlgItem(dialog(),1001),L"A\U0001F680B");click(IDOK);pump_guest();unload(0,2);
    CHECK(vm_read16(0x2200)=='A' && !vm_read16(0x2202) && vm_read16(0x2204)==0xA5A5);
    /* Two guest lifecycles can finish before the owner pumps either close. */
    CHECK(!call(0x7FCFC915,UINT64_MAX,0x1000,0x1200));opened();
    CHECK(!call(0xB6D84526,0,0,0));pump_guest();result_buffer(4);
    CHECK(!call(0x3D1E1931,0x1300,0,0));pump_guest();CHECK(dialog()!=NULL);
    CHECK(!call(0x7FCFC915,UINT64_MAX,0x1000,0x1200));
    CHECK(!call(0xB6D84526,0,0,0));pump_guest();result_buffer(4);
    CHECK(!call(0x3D1E1931,0x1300,0,0));pump_guest();pump_host();
    CHECK(!dialog() && !osk_native_win32_input_active());
    /* A host request delayed past its close cannot resurrect input ownership. */
    OskNativeRequest stale={0};stale.token=1;
    osk_native_open(&stale);pump_host();CHECK(!dialog() && !osk_native_win32_input_active());
    CHECK(!call(0x7FCFC915,UINT64_MAX,0x1000,0x1200));
    osk_native_open(&stale);opened();
    if(argc>2 && !strcmp(argv[1],"control")){
        command_file(argv[2],1);pump_guest();CHECK(dialog()!=NULL);
        CHECK(GetFileAttributesA(argv[2])==INVALID_FILE_ATTRIBUTES);
        command_file(argv[2],loaded_generation);pump_guest();CHECK(dialog()!=NULL);
    }else osk_native_confirm(loaded_generation);
    osk_native_confirm(1);pump_host();
    CHECK(!dialog());pump_guest();CHECK(statuses[events-1]==0x503);unload(0,4);
    /* A guest worker returns before the owner thread creates the real UI. */
    vm_write32(0x1208,16);HANDLE worker=CreateThread(NULL,0,worker_load,NULL,0,NULL);CHECK(worker!=NULL);
    CHECK(WaitForSingleObject(worker,2000)==WAIT_OBJECT_0);DWORD worker_result=1;CHECK(GetExitCodeThread(worker,&worker_result));CloseHandle(worker);
    CHECK(!worker_result && !dialog());opened();click(IDCANCEL);pump_guest();unload(1,4);
    vm_write32(0x1208,0);CHECK(!call(0x7FCFC915,UINT64_MAX,0x1000,0x1200));opened();
    CHECK(GetWindowLongPtrW(GetDlgItem(dialog(),1001),GWL_STYLE)&ES_READONLY);
    SetWindowTextW(GetDlgItem(dialog(),1001),L"ignored");click(IDOK);pump_guest();unload(0,0);
    CHECK(!vm_read16(0x2200) && vm_read16(0x2202)==0xA5A5);
    vm_write32(0x1208,16);
    /* Closing the real parent aborts an open dialog through the guest pump. */
    CHECK(!call(0x7FCFC915,UINT64_MAX,0x1000,0x1200));opened();count=events;
    DestroyWindow(parent);parent=NULL;pump_host();CHECK(!dialog() && events==count && !osk_native_available());
    pump_guest();CHECK(statuses[events-1]==0x503);unload(2,4);
#endif
    if(parent)DestroyWindow(parent);printf("PASS Windows native OSK: %u checks\n",checks);return 0;
}

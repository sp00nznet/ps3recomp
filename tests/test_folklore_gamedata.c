/* Compile this standalone C fixture; it tests the real implementation with a
 * synthetic synchronous guest callback and small synthetic PARAM.SFO files. */
#include "../libs/system/cellGame.c"
#include <process.h>
uint8_t* vm_base;
uint32_t ppu_vm_size=0x02600000;
const char* ppu_vfs_root=".";
ps3_guest_caller_fn g_ps3_guest_caller;
/* No SPU reservations or memory watchpoints in this isolated memory fixture. */
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
int spu_coh_is_reserved(uint32_t a){(void)a;return 0;}
void spu_lockline_lock(void){}
void spu_lockline_unlock(void){}
void spu_coh_notify_write(uint32_t a){(void)a;}
void ppu_resv_break_store(uint64_t a){(void)a;}
static int failures, checks, calls, expected_new=1, action=1;
static void check(int yes,const char* name){++checks;if(!yes){++failures;printf("FAIL %s\n",name);}}
static void callback(uint32_t opd,uint64_t cb,uint64_t get,uint64_t set,
                     uint64_t a3,uint64_t a4,uint64_t a5,uint64_t a6,uint64_t a7)
{
    (void)opd;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;++calls;
    check(set>=get+sizeof(CellGameDataStatGet),"stat objects disjoint");
    check(vm_read32((u32)get+4)==(u32)expected_new,"disk-derived isNewData");
    check(vm_read32((u32)get+0x13FC)==UINT32_MAX,"sizeKB offset");
    check(vm_read32((u32)get+0x1400)==0,"sysSizeKB offset");
    check(vm_read32((u32)get)>0,"actual volume free space");
    check(!strcmp((char*)vm_base+get+8,"/dev_hdd0/game/TEST12345"),"content path");
    check(!strcmp((char*)vm_base+get+0x427,"/dev_hdd0/game/TEST12345/USRDIR"),"data path");
    u32 gp=(u32)get+0x860;
    if(expected_new){
        int zero=1;for(size_t i=0;i<sizeof(CellGameDataSystemFileParam);++i)if(vm_base[gp+i])zero=0;
        check(zero,"new data has no fabricated metadata");
    }else{
        check(!strcmp((char*)vm_base+gp,"Synthetic disk title"),"disk title");
        check(!strcmp((char*)vm_base+gp+0xA80,"TEST12345"),"title ID offset");
        check(!strcmp((char*)vm_base+gp+0xA8C,"02.03"),"APP_VER precedence");
        check(vm_read32(gp+0xA94)==5,"parental integer BE");
    }
    if(action==3)cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0);
    vm_write32((u32)cb,action==4 ? 2 : action==2 || action==6 ? 0 : 1);
    if(action==2)vm_write32((u32)set,0x3000);
}
static void le32(unsigned char*p,u32 v){p[0]=(u8)v;p[1]=(u8)(v>>8);p[2]=(u8)(v>>16);p[3]=(u8)(v>>24);}
static void make_sfo(const char*path,int broken){
    unsigned char d[1024]={0};memcpy(d,"\0PSF",4);le32(d+4,0x101);
    const char*keys[]={"CATEGORY","TITLE","TITLE_ID","VERSION","APP_VER","PARENTAL_LEVEL"};
    const char*values[]={"GD","Synthetic disk title","TEST12345","01.02","02.03",NULL};
    u32 keybase=20+6*16, valuebase=256, ko=0, vo=0;
    le32(d+8,keybase);le32(d+12,valuebase);le32(d+16,6);
    for(u32 i=0;i<6;++i){
        unsigned char*e=d+20+i*16;e[0]=(u8)ko;e[1]=(u8)(ko>>8);e[2]=4;e[3]=i==5?4:2;
        u32 len=i==5?4:(u32)strlen(values[i])+1;
        le32(e+4,len);le32(e+8,len);le32(e+12,vo);
        strcpy((char*)d+keybase+ko,keys[i]);ko+=(u32)strlen(keys[i])+1;
        if(i==5)le32(d+valuebase+vo,5);else memcpy(d+valuebase+vo,values[i],len);vo+=len;
    }
    FILE*f=fopen(path,"wb");if(!f)exit(2);fwrite(d,1,broken?21:valuebase+vo,f);fclose(f);
}
int main(int argc,char**argv){
    if(argc<2)return 2;
    vm_base=calloc(1,ppu_vm_size);if(!vm_base)return 2;
    strcpy((char*)vm_base+0x1000,"TEST12345");g_ps3_guest_caller=callback;
    cellGame_set_content_path(argv[1]);
    _putenv_s("PS3_STRICT_FS_READONLY","");_putenv_s("PS3_STRICT_FS_WRITE_ROOT","");
    _putenv_s("PS3_STRICT_BOOT",argc>2?"1":"0");
    if(argc>2){action=atoi(argv[2]);if(action==5)g_ps3_guest_caller=NULL;return cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0);}
    char dir[1024],path[1024];_mkdir(argv[1]);snprintf(dir,sizeof dir,"%s/TEST12345",argv[1]);_mkdir(dir);snprintf(path,sizeof path,"%s/PARAM.SFO",dir);
    u32 end=GAMEDATA_CB_BASE+0x20+sizeof(CellGameDataStatGet)+sizeof(CellGameDataStatSet)+sizeof(CellGameDataSystemFileParam);
    memset(vm_base+GAMEDATA_CB_BASE-16,0xA5,16);memset(vm_base+end,0xA5,16);
    check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==0,"new check-only callback");
    check(calls==1,"callback invoked once");
    int canary=1;for(int i=0;i<16;++i)if(vm_base[GAMEDATA_CB_BASE-16+i]!=0xA5||vm_base[end+i]!=0xA5)canary=0;check(canary,"scratch boundary canaries");
    make_sfo(path,0);expected_new=0;
    check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==0,"existing metadata check");
    action=6;check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==0,"existing metadata unchanged success");action=1;
    action=4;check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_CBRESULT,"unknown callback result rejected");action=1;
    make_sfo(path,1);int before=calls;
    check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_BROKEN,"malformed SFO rejected");check(calls==before,"no callback for broken SFO");
    remove(path);expected_new=1;
    action=6;check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_PARAM,"new data requires setParam");action=1;
    check(cellGameDataCheckCreate(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==0,"legacy entry uses same ABI");
    check(cellHddGameCheck(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==1,"HDD cancel return retained");
    check(cellGameDataCheckCreate2(1,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_PARAM,"version validation");
    strcpy((char*)vm_base+0x1000,"../escape");check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_PARAM,"directory traversal rejected");strcpy((char*)vm_base+0x1000,"TEST12345");
    const char*modes[]={"2","3","5"};for(int i=0;i<3;++i)check(_spawnl(_P_WAIT,argv[0],argv[0],argv[1],modes[i],NULL)==86,"strict unsupported operation exits 86");
    printf("RESULT checks=%d failures=%d\n",checks,failures);free(vm_base);return failures?1:0;
}

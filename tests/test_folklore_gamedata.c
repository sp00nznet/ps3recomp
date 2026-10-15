/* Compile this standalone C fixture; it tests the real implementation with a
 * synthetic synchronous guest callback and small synthetic PARAM.SFO files. */
#include "../libs/system/cellGame.c"
#ifdef _WIN32
#include <process.h>
static int fixture_spawn(const char* exe,const char* folder,const char* mode)
{return (int)_spawnl(_P_WAIT,exe,exe,folder,mode,NULL);}
#else
#include <sys/wait.h>
#define _putenv_s(name,value) setenv(name,value,1)
#define _mkdir(path) mkdir(path,0755)
static int fixture_spawn(const char* exe,const char* folder,const char* mode)
{
    pid_t child=fork();if(child<0)return -1;
    if(!child){execl(exe,exe,folder,mode,(char*)NULL);_Exit(127);}
    int status=0;if(waitpid(child,&status,0)!=child)return -1;
    return WIFEXITED(status)?WEXITSTATUS(status):-1;
}
#endif
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
static const char* collision_path;
static const char* expected_title="Synthetic disk title";
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
        check(!strcmp((char*)vm_base+gp,expected_title),"disk title");
        check(!strcmp((char*)vm_base+gp+0xA80,"TEST12345"),"title ID offset");
        check(!strcmp((char*)vm_base+gp+0xA8C,"02.03"),"APP_VER precedence");
        check(vm_read32(gp+0xA94)==5,"parental integer BE");
    }
    if(action==3)cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0);
    vm_write32((u32)cb,action==4 ? 2 : action==2 || action==6 || action==8 || action==9 || action==12 || action==13 || action==14 || action==15 ? 0 : 1);
    if(action==2||action==8||action==12||action==13){
        u32 p=0x13000;
        strcpy((char*)vm_base+p,action==8?"Updated title":"Synthetic disk title");
        strcpy((char*)vm_base+p+0xA80,"TEST12345");
        strcpy((char*)vm_base+p+0xA8C,"02.03");
        strcpy((char*)vm_base+p+0x80,"English title");
        vm_write32(p+0xA94,5);
        vm_write32((u32)set,p);
        if(action==13)memset(vm_base+p,'x',128);
    }
    if(action==9)vm_write32((u32)set,ppu_vm_size-1);
    if(action==14)vm_write32((u32)set,1);
    if(action==15)vm_write32((u32)set,0xFFFF);
    if(action==12){FILE*f=fopen(collision_path,"wb");if(!f)_Exit(2);fputs("competing creator",f);fclose(f);}
}
static void le32(unsigned char*p,u32 v){p[0]=(u8)v;p[1]=(u8)(v>>8);p[2]=(u8)(v>>16);p[3]=(u8)(v>>24);}
static void make_sfo(const char*path,int broken){
    unsigned char d[1024]={0};memcpy(d,"\0PSF",4);le32(d+4,0x101);
    const char*keys[]={"CATEGORY","TITLE","TITLE_ID","VERSION","APP_VER","PARENTAL_LEVEL","EXTRA"};
    const char*values[]={"GD","Synthetic disk title","TEST12345","01.02","02.03",NULL,"marker"};
    u32 keybase=20+7*16, valuebase=256, ko=0, vo=0;
    le32(d+8,keybase);le32(d+12,valuebase);le32(d+16,7);
    for(u32 i=0;i<7;++i){
        vo=(vo+3)&~3u;
        unsigned char*e=d+20+i*16;e[0]=(u8)ko;e[1]=(u8)(ko>>8);e[2]=4;e[3]=i==5?4:2;
        u32 len=i==5?4:(u32)strlen(values[i])+1;
        le32(e+4,len);le32(e+8,len);le32(e+12,vo);
        strcpy((char*)d+keybase+ko,keys[i]);ko+=(u32)strlen(keys[i])+1;
        if(i==5)le32(d+valuebase+vo,5);else memcpy(d+valuebase+vo,values[i],len);vo+=len;
    }
    FILE*f=fopen(path,"wb");if(!f)exit(2);fwrite(d,1,broken?21:valuebase+vo,f);fclose(f);
}
static unsigned long file_hash(const char* path){
    FILE*f=fopen(path,"rb");if(!f)return 0;
    unsigned long h=2166136261u;int c;while((c=fgetc(f))!=EOF)h=(h^(unsigned char)c)*16777619u;
    fclose(f);return h;
}
static int snapshot_file(const char* from,const char* to){
    FILE*in=fopen(from,"rb");if(!in)return 0;
    FILE*out=fopen(to,"wbx");if(!out){fclose(in);return 0;}
    unsigned char buffer[4096];size_t n;int ok=1;
    while((n=fread(buffer,1,sizeof buffer,in))!=0)if(fwrite(buffer,1,n,out)!=n){ok=0;break;}
    if(ferror(in))ok=0;
    fclose(in);if(fclose(out))ok=0;return ok;
}
int main(int argc,char**argv){
    if(argc<2)return 2;
    vm_base=calloc(1,ppu_vm_size);if(!vm_base)return 2;
    strcpy((char*)vm_base+0x1000,"TEST12345");g_ps3_guest_caller=callback;
    cellGame_set_content_path(argv[1]);
    _putenv_s("PS3_STRICT_FS_READONLY","");_putenv_s("PS3_STRICT_FS_WRITE_ROOT","");
    _putenv_s("PS3_STRICT_BOOT",argc>2?"1":"0");
    if(argc>2){action=atoi(argv[2]);if(action==5)g_ps3_guest_caller=NULL;
        if(action==7){action=2;_putenv_s("PS3_STRICT_FS_READONLY","1");}
        if(action==10){char allowed[1024];snprintf(allowed,sizeof allowed,"%s/allowed",argv[1]);
            action=2;_putenv_s("PS3_STRICT_FS_WRITE_ROOT",allowed);}
        if(action==11){action=8;expected_new=0;expected_title="Updated title";
            _putenv_s("PS3_STRICT_FS_READONLY","1");}
        return cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0);}
#ifdef _WIN32
    _putenv_s("PS3_STRICT_FS_WRITE_ROOT",argv[1]);
#endif
    char dir[1024],path[1024];_mkdir(argv[1]);snprintf(dir,sizeof dir,"%s/TEST12345",argv[1]);_mkdir(dir);snprintf(path,sizeof path,"%s/PARAM.SFO",dir);
    u32 end=GAMEDATA_CB_BASE+0x20+sizeof(CellGameDataStatGet)+sizeof(CellGameDataStatSet)+sizeof(CellGameDataSystemFileParam);
    memset(vm_base+GAMEDATA_CB_BASE-16,0xA5,16);memset(vm_base+end,0xA5,16);
    check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==0,"new check-only callback");
    check(calls==1,"callback invoked once");
    int canary=1;for(int i=0;i<16;++i)if(vm_base[GAMEDATA_CB_BASE-16+i]!=0xA5||vm_base[end+i]!=0xA5)canary=0;check(canary,"scratch boundary canaries");
    check(file_hash(path)==0,"cancel leaves no SFO");
    action=2;check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==0,"new data persisted");action=1;
    char usrdir[1024];snprintf(usrdir,sizeof usrdir,"%s/USRDIR",dir);
    check(dir_exists(usrdir),"requested USRDIR exists");
    check(file_hash(path)!=0,"real SFO written");
    expected_new=0;
    check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==0,"created SFO reopens");
    make_sfo(path,0);expected_new=0;
    check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==0,"existing metadata check");
    unsigned long old_hash=file_hash(path);
    action=6;check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==0,"existing metadata unchanged success");action=1;
    check(file_hash(path)==old_hash,"null setParam leaves existing SFO unchanged");
    action=8;check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==0,"existing SFO updated");action=1;
    expected_title="Updated title";
    check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==0,"updated title reopens");
    char extra[32]={0};check(sfo_read_string(path,"EXTRA",extra,sizeof extra)==0&&!strcmp(extra,"marker"),"unknown SFO key preserved");
    char updated_snapshot[1024];snprintf(updated_snapshot,sizeof updated_snapshot,"%s/updated.SFO",argv[1]);
    check(snapshot_file(path,updated_snapshot),"updated SFO retained for independent parser");
    old_hash=file_hash(path);action=9;
    check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_PARAM,"invalid setParam rejected");action=1;
    check(file_hash(path)==old_hash,"invalid update leaves old SFO unchanged");
    check(fixture_spawn(argv[0],argv[1],"11")==86,"readonly update stops before write");
    check(file_hash(path)==old_hash,"readonly update leaves old SFO unchanged");
    action=4;check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_CBRESULT,"unknown callback result rejected");action=1;
    make_sfo(path,1);int before=calls;
    check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_BROKEN,"malformed SFO rejected");check(calls==before,"no callback for broken SFO");
    remove(path);expected_new=1;expected_title="Synthetic disk title";
    action=6;check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_PARAM,"new data requires setParam");action=1;
    check(cellGameDataCheckCreate(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==0,"legacy entry uses same ABI");
    check(cellHddGameCheck(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==1,"HDD cancel return retained");
    check(cellGameDataCheckCreate2(1,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_PARAM,"version validation");
    strcpy((char*)vm_base+0x1000,"../escape");check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_PARAM,"directory traversal rejected");strcpy((char*)vm_base+0x1000,"TEST12345");
    char allowed[1024];snprintf(allowed,sizeof allowed,"%s/allowed",argv[1]);_mkdir(allowed);
    const char*modes[]={"3","5","7","10"};for(int i=0;i<4;++i)check(fixture_spawn(argv[0],argv[1],modes[i])==86,"strict unsupported or out-of-root operation exits 86");
    action=13;check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_PARAM,"unterminated title rejected");
    check(file_hash(path)==0,"invalid new metadata creates no SFO");
    action=14;check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_PARAM,"protected low setParam rejected");
    action=15;check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_PARAM,"last protected low setParam rejected");
    collision_path=path;action=12;
    check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==CELL_GAMEDATA_ERROR_ACCESS_ERROR,"competing creator not overwritten");
    char competing[32]={0};FILE* competing_file=fopen(path,"rb");
    if(competing_file){fread(competing,1,sizeof competing-1,competing_file);fclose(competing_file);}
    check(!strcmp(competing,"competing creator"),"competing file bytes preserved");
    remove(path);action=2;
    check(cellGameDataCheckCreate2(0,(const char*)(uintptr_t)0x1000,0,(void*)(uintptr_t)0x2000,0)==0,"final new metadata retained for independent parse");
    printf("RESULT checks=%d failures=%d\n",checks,failures);free(vm_base);return failures?1:0;
}

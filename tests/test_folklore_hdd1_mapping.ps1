param([string]$Repo = 'Q:\folklore\work\ps3recomp-strict', [string]$Work = ('Q:\folklore\work\hdd1-mapping-' + [Guid]::NewGuid().ToString('N')))
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Path $Work | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Work 'hdd1/cache') -Force | Out-Null
function Slice($path,$start,$end) { $s=[IO.File]::ReadAllText((Join-Path $Repo $path)); $a=$s.IndexOf($start); $b=$s.IndexOf($end,$a); if($a -lt 0 -or $b -lt 0){throw 'Source extraction boundary missing'}; $s.Substring($a,$b-$a) }
$ppu=Slice 'runtime/ppu/ppu_fs.cpp' 'static void host_path(' '/* ---- fd / dir handle tables'
$sys=Slice 'runtime/syscalls/sys_fs.c' 'void sys_fs_translate_path(' '/* ---------------------------------------------------------------------------'
$norm=Slice 'runtime/syscalls/sys_fs.c' 'static void fs_normalize_sep(' '/* Extracted game trees'
$lib=Slice 'libs/filesystem/cellFs.c' '#define MAX_PATH_MAPPINGS' '/* Public wrapper for translate_path'
$pre=@'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "ps3emu/strict_fs.h"
#define CELL_FS_MAX_FS_PATH_LENGTH 1024
static int fallback_calls;
void ps3_vfs_ps3game_fallback(char*,size_t) { ++fallback_calls; }
void cellfs_add_path_mapping(const char*,const char*);
const char* ppu_vfs_root="Q:/synthetic-disc";
char g_sys_fs_root[512]="Q:/synthetic-disc";
'@
$main=@'
static int failures, checks;
static void check(int yes,const char* label){++checks;if(!yes){++failures;printf("FAIL %s\n",label);}}
static void slash(char* p){for(;*p;++p)if(*p=='\\')*p='/';}
static void map(int which,const char* guest,char* out,size_t size) {
 if(which==0)host_path(out,size,guest);
 if(which==1)sys_fs_translate_path(guest,out,(int)size);
 if(which==2)translate_path(guest,out,size);
 slash(out);
}
int main(int argc,char**argv){
 if(argc!=2)return 2;
 _putenv_s("PS3_HDD0_ROOT","");_putenv_s("PS3_USRDIR_BASE","");_putenv_s("PS3_HDD1_ROOT","");
 cellfs_set_root_path("Q:/synthetic-disc");
 cellfs_add_path_mapping("/dev_hdd1/","registered/");
 cellfs_add_path_mapping("/dev_hdd1/cache/","specific/");
 char out[1024],expect[1024],root[1024],a[1024],b[1024];
 snprintf(root,sizeof root,"%s/hdd1",argv[1]);slash(root);
 for(int mode=0;mode<3;++mode){
   map(mode,"/dev_hdd1/cache/file",out,sizeof out);
   check(!strcmp(out,mode==2?"Q:/synthetic-disc/specific/file":"Q:/synthetic-disc/cache/file"),"unset preserves mapping");
 }
 _putenv_s("PS3_STRICT_FS_READONLY","0");_putenv_s("PS3_STRICT_FS_WRITE_ROOT",root);
 for(int trailing=0;trailing<2;++trailing){
   snprintf(a,sizeof a,"%s%s",root,trailing?"/":"");_putenv_s("PS3_HDD1_ROOT",a);
   for(int mode=0;mode<3;++mode){
     int prior=fallback_calls;
     map(mode,"/dev_hdd1",out,sizeof out);check(!strcmp(out,root),"bare mount");
     map(mode,"/dev_hdd1/",out,sizeof out);check(!strcmp(out,root),"slash mount");
     map(mode,"/dev_hdd1/cache/file",out,sizeof out);snprintf(expect,sizeof expect,"%s/cache/file",root);check(!strcmp(out,expect),"override wins nested mapping");
     map(mode,"/dev_hdd1/game/X/USRDIR/file",out,sizeof out);snprintf(expect,sizeof expect,"%s/game/X/USRDIR/file",root);check(!strcmp(out,expect),"no USRDIR flattening");
     check(fallback_calls==prior,"no asset fallback");
     map(mode,"/dev_hdd1/cache/file",out,4);check(!out[0],"truncation fails closed");
     map(mode,"/dev_hdd10/cache/file",out,sizeof out);check(strncmp(out,root,strlen(root))!=0,"mount boundary");
   }
 }
 for(int writer=0;writer<3;++writer){
   map(writer,"/dev_hdd1/cache/shared.txt",a,sizeof a);
   FILE*f=ps3_strict_fs_fopen(a,"wb");check(f!=NULL,"mapped write open");if(!f)continue;
   check(ps3_strict_fs_fwrite("mounted",1,7,f)==7,"mapped write");fclose(f);
   for(int reader=0;reader<3;++reader){map(reader,"/dev_hdd1/cache/shared.txt",b,sizeof b);f=fopen(b,"rb");char data[8]={0};if(f){fread(data,1,7,f);fclose(f);}check(!strcmp(data,"mounted"),"cross-translator read");}
 }
 _putenv_s("PS3_HDD1_ROOT","");
 for(int mode=0;mode<3;++mode){map(mode,"/dev_hdd1/cache/file",out,sizeof out);check(!strcmp(out,mode==2?"Q:/synthetic-disc/specific/file":"Q:/synthetic-disc/cache/file"),"empty restores previous mapping");}
 printf("RESULT checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
'@
$source=Join-Path $Work 'mapping_contract.cpp'
[IO.File]::WriteAllText($source,($pre+"`n"+$norm+"`n"+$ppu+"`n"+$sys+"`n"+$lib+"`n"+$main))
$env:PATH='D:\Tools\msys64\clang64\bin;'+$env:PATH
$exe=Join-Path $Work 'mapping_contract.exe'
& 'D:\Tools\msys64\clang64\bin\clang++.exe' -std=c++20 -I (Join-Path $Repo 'include') $source -o $exe
if($LASTEXITCODE -ne 0){throw 'Compile failed'}
& $exe $Work
if($LASTEXITCODE -ne 0){throw 'Contract tests failed'}
Write-Output "EVIDENCE_ROOT=$Work"

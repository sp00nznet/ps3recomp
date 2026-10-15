/* Synthetic bytes, real acquire/capture only. Fresh empty DIR per process. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "spurs_job_snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#define T(x) do{if(!(x)){fprintf(stderr,"SPAN_CAPTURE_FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
static void env(const char*k,const char*v){
#ifdef _WIN32
 T(_putenv_s(k,v?v:"")==0);
#else
 T((v?setenv(k,v,1):unsetenv(k))==0);
#endif
}
static void protection(uint8_t*p,unsigned n,int readable,int guard){
#ifdef _WIN32
 DWORD old;T(VirtualProtect(p,n,readable?(guard?PAGE_READWRITE|PAGE_GUARD:PAGE_READONLY):PAGE_NOACCESS,&old));
#else
 T(mprotect(p,n,guard?PROT_NONE:readable?PROT_READ:PROT_NONE)==0);
#endif
}
static int exists(const char*p){FILE*f=fopen(p,"rb");if(!f)return 0;fclose(f);return 1;}
static void bytes(const char*p,const void*want,unsigned n){FILE*f=fopen(p,"rb");T(f);uint8_t*b=(uint8_t*)malloc(n);T(b);T(fread(b,1,n,f)==n&&fgetc(f)==EOF&&!memcmp(b,want,n));T(fclose(f)==0);free(b);}
static void metadata(const char*p,unsigned schema,int spans){char path[4096],buf[2048];snprintf(path,sizeof path,"%s.json",p);FILE*f=fopen(path,"rb");T(f);size_t n=fread(buf,1,sizeof buf-1,f);T(!ferror(f)&&feof(f));buf[n]=0;T(fclose(f)==0);char marker[64];snprintf(marker,sizeof marker,"\"schema_version\": %u",schema);T(strstr(buf,marker));T((strstr(buf,"capture_spans")!=NULL)==spans);T(!strstr(buf,"auxiliary_"));}
int main(int argc,char**argv)
{
 if(argc!=3)return 2;const char*mode=argv[1];const char*dir=argv[2];T(strlen(dir)<3000);
 const char*known[]={"full","single","max","selected","mixed","index-alone","guest-limit","unreadable-middle","guard","collision","collision-second","readonly","count-budget","byte-budget","invalid-budget","disabled"};
 int named=0;for(unsigned i=0;i<sizeof known/sizeof*known;i++)if(!strcmp(mode,known[i]))named=1;
 if(!named&&strncmp(mode,"invalid-",8)&&strncmp(mode,"index-bad-",10))return 2;
 env("FOLKLORE_SPU_CAPTURE_DIR",dir);env("FOLKLORE_SPU_CAPTURE_AUX_EA",NULL);
 env("FOLKLORE_SPU_CAPTURE_SPANS",NULL);env("FOLKLORE_SPU_CAPTURE_SPANS_INDEX",NULL);
 char out[4096]="old",leaf[4096],path[4096];
 if(!strcmp(mode,"disabled")){env("FOLKLORE_SPU_CAPTURE_DIR",NULL);env("FOLKLORE_SPU_CAPTURE_SPANS","bad");env("FOLKLORE_SPU_CAPTURE_SPANS_INDEX","bad");T(spurs_job_snapshot_capture(NULL,out,sizeof out)==SPURS_JOB_CAPTURE_DISABLED&&!out[0]);puts("SPAN_CAPTURE_PASS disabled");return 0;}
#ifdef _WIN32
 uint8_t*vm=(uint8_t*)VirtualAlloc(NULL,0x100000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);SYSTEM_INFO si;GetSystemInfo(&si);unsigned page=si.dwPageSize;
#else
 uint8_t*vm=(uint8_t*)mmap(NULL,0x100000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);T(vm!=MAP_FAILED);unsigned page=(unsigned)sysconf(_SC_PAGESIZE);
#endif
 T(vm&&page<=65536&&!(0x80000%page));
 for(unsigned i=0;i<0x100000;i++)vm[i]=(uint8_t)(i*37u+(i>>8));
 memset(vm+0x10000,0,1024);vm[0x10005]=2; /* image EA20000 */
 unsigned large=!strcmp(mode,"byte-budget"),image_size=large?262144:16,desc_size=large?1024:128;
 vm[0x10008]=(uint8_t)((image_size/16)>>8);vm[0x10009]=(uint8_t)(image_size/16);
 spurs_job_snapshot s={0};T(spurs_job_snapshot_acquire(vm,0x100000,0x10000,desc_size,&s)==SPURS_JOB_SNAPSHOT_OK);
 snprintf(leaf,sizeof leaf,"%s/spurs-job-%08X-%016llX-00",dir,s.job_ea,(unsigned long long)s.fingerprint);
 const char*spec="0x80000:32,0x90000:84048";unsigned span_n=2,n0=32,n1=84048;
 if(!strcmp(mode,"single")){spec="0x80000:1";span_n=1;n0=1;}
 if(!strcmp(mode,"max")||large){spec="0x80000:131072";span_n=1;n0=131072;}
 env("FOLKLORE_SPU_CAPTURE_SPANS",spec);
 int invalid=0;
 static const char*bad[]={"0xFFFF:1","0x100000000:1","0xFFFFFFFF:2","0x80000:0","0x80000:01","0x80000:131073","0x80000:1,","0x80000:1,0x90000:1,0xA0000:1","0x80000:2,0x80001:2","0x80000:65537,0xA0000:65536","0x80000:1 "," 0x80000:1","0x80000:","0x80000:9999999999999999999999"};
 if(!strncmp(mode,"invalid-",8)&&strcmp(mode,"invalid-budget")){char*end;unsigned k=(unsigned)strtoul(mode+8,&end,10);if(end==mode+8||*end||k>=sizeof bad/sizeof*bad)return 2;env("FOLKLORE_SPU_CAPTURE_SPANS",bad[k]);invalid=1;}
 if(!strcmp(mode,"mixed")){env("FOLKLORE_SPU_CAPTURE_AUX_EA","0x80000");invalid=1;}
 if(!strcmp(mode,"index-alone")){env("FOLKLORE_SPU_CAPTURE_SPANS",NULL);env("FOLKLORE_SPU_CAPTURE_SPANS_INDEX","2");invalid=1;}
 if(!strncmp(mode,"index-bad-",10)){const char*badindex[]={"00","16","-1","1 ","1,2","999999"};char*end;unsigned k=(unsigned)strtoul(mode+10,&end,10);if(end==mode+10||*end||k>=6)return 2;env("FOLKLORE_SPU_CAPTURE_SPANS_INDEX",badindex[k]);invalid=1;}
 if(!strcmp(mode,"guest-limit")){s.guest_limit=0x90001;invalid=1;}
 int selected=!strcmp(mode,"selected");
 if(selected){env("FOLKLORE_SPU_CAPTURE_SPANS_INDEX","2");protection(vm+0x80000,0x20000,0,0);for(unsigned i=0;i<2;i++){T(spurs_job_snapshot_capture(&s,out,sizeof out)==SPURS_JOB_CAPTURE_WRITTEN);metadata(out,1,0);} 
#ifdef _WIN32
 DWORD old;T(VirtualProtect(vm+0x80000,0x20000,PAGE_READWRITE,&old));
#else
 T(mprotect(vm+0x80000,0x20000,PROT_READ|PROT_WRITE)==0);
#endif
 }
 int protected_span=!strcmp(mode,"unreadable-middle")||!strcmp(mode,"guard");
 if(protected_span){env("FOLKLORE_SPU_CAPTURE_SPANS","0x80000:131072");protection(vm+0x80000+page,page,1,!strcmp(mode,"guard"));if(!strcmp(mode,"unreadable-middle"))protection(vm+0x80000+page,page,0,0);invalid=1;}
 int collision=!strcmp(mode,"collision")||!strcmp(mode,"collision-second");unsigned collision_index=!strcmp(mode,"collision-second");
 if(collision){snprintf(path,sizeof path,"%s.span-%02u.bin",leaf,collision_index);FILE*f=fopen(path,"wb");T(f&&fwrite("KEEP",1,4,f)==4&&fclose(f)==0);invalid=1;}
 if(!strcmp(mode,"readonly"))protection(vm+0x80000,0x30000,1,0);
 unsigned writes=0;
 if(!strcmp(mode,"invalid-budget")){env("FOLKLORE_SPU_CAPTURE_SPANS","bad");for(unsigned i=0;i<16;i++)T(spurs_job_snapshot_capture(&s,out,sizeof out)==SPURS_JOB_CAPTURE_ERROR&&!out[0]);T(spurs_job_snapshot_capture(&s,out,sizeof out)==SPURS_JOB_CAPTURE_LIMIT&&!out[0]);}
 else if(!strcmp(mode,"count-budget")||large){for(unsigned i=0;i<17;i++){spurs_job_capture_status rc=spurs_job_snapshot_capture(&s,out,sizeof out);if(rc==SPURS_JOB_CAPTURE_LIMIT){T(!out[0]);break;}T(rc==SPURS_JOB_CAPTURE_WRITTEN);++writes;}T(writes==(large?10u:16u));}
 else{
  spurs_job_capture_status rc=spurs_job_snapshot_capture(&s,out,sizeof out);
  if(invalid){T(rc==SPURS_JOB_CAPTURE_ERROR&&!out[0]);snprintf(path,sizeof path,"%s.desc.bin",leaf);T(!exists(path));snprintf(path,sizeof path,"%s.image.bin",leaf);T(!exists(path));snprintf(path,sizeof path,"%s.json",leaf);T(!exists(path));for(unsigned i=0;i<2;i++){snprintf(path,sizeof path,"%s.span-%02u.bin",leaf,i);if(collision&&i==collision_index)bytes(path,"KEEP",4);else T(!exists(path));}}
  else{T(rc==SPURS_JOB_CAPTURE_WRITTEN);metadata(out,3,1);snprintf(path,sizeof path,"%s.span-00.bin",out);bytes(path,vm+0x80000,n0);if(span_n==2){snprintf(path,sizeof path,"%s.span-01.bin",out);bytes(path,vm+0x90000,n1);}snprintf(path,sizeof path,"%s.desc.bin",out);bytes(path,s.descriptor,s.descriptor_size);}
 }
#ifdef _WIN32
 if(!strcmp(mode,"guard")){MEMORY_BASIC_INFORMATION info;T(VirtualQuery(vm+0x80000+page,&info,sizeof info)==sizeof info&&(info.Protect&PAGE_GUARD));}
#endif
 /* No capture path owns a guest write. Restore protection before readback/free. */
#ifdef _WIN32
 DWORD restore;T(VirtualProtect(vm,0x100000,PAGE_READWRITE,&restore));
#else
 T(mprotect(vm,0x100000,PROT_READ|PROT_WRITE)==0);
#endif
 for(unsigned i=0x80000;i<0xB0000;i++)T(vm[i]==(uint8_t)(i*37u+(i>>8)));
 spurs_job_snapshot_release(&s);
#ifdef _WIN32
 T(VirtualFree(vm,0,MEM_RELEASE));
#else
 T(munmap(vm,0x100000)==0);
#endif
 printf("SPAN_CAPTURE_PASS mode=%s writes=%u\n",mode,writes);return 0;
}

/* Synthetic production-helper fixture, no replaced functions. Each invocation
 * needs a fresh existing absolute directory. Link spurs_job_snapshot.c.
 * Modes: disabled, default, valid, readonly, guard, edge, invalid, aux-collision, meta-collision.
 * Run each mode in a separate process (capture counters are process lifetime). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "spurs_job_snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <dirent.h>
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define LIMIT UINT64_C(0x100000000)
#define JOB 0x20000u
#define IMAGE 0x30000u
#define AUX 0x4501B900u
static unsigned char* memory;
static size_t page;
static void environment(const char* name,const char* value)
{
#ifdef _WIN32
    CHECK(_putenv_s(name,value ? value : "")==0);
#else
    CHECK(value ? setenv(name,value,1)==0 : unsetenv(name)==0);
#endif
}
static void commit(uint32_t ea)
{
#ifdef _WIN32
    CHECK(VirtualAlloc(memory+ea,page,MEM_COMMIT,PAGE_READWRITE)==memory+ea);
#else
    CHECK(mprotect(memory+ea,page,PROT_READ|PROT_WRITE)==0);
#endif
}
static int exists(const char* path)
{
    FILE* f=fopen(path,"rb"); if (!f) return 0; CHECK(fclose(f)==0); return 1;
}
static unsigned files(const char* directory)
{
    unsigned count=0;
#ifdef _WIN32
    char path[4096]; CHECK(snprintf(path,sizeof path,"%s/*",directory)>0);
    WIN32_FIND_DATAA data; HANDLE find=FindFirstFileA(path,&data);
    if (find==INVALID_HANDLE_VALUE) return 0;
    do { if (strcmp(data.cFileName,".") && strcmp(data.cFileName,"..")) ++count; } while (FindNextFileA(find,&data));
    CHECK(FindClose(find));
#else
    DIR* dir=opendir(directory); CHECK(dir); struct dirent* item;
    while ((item=readdir(dir))) if (strcmp(item->d_name,".") && strcmp(item->d_name,"..")) ++count;
    CHECK(closedir(dir)==0);
#endif
    return count;
}
static size_t read_file(const char* prefix,const char* suffix,void* out,size_t capacity)
{
    char path[4096]; CHECK(snprintf(path,sizeof path,"%s%s",prefix,suffix)>0);
    FILE* f=fopen(path,"rb"); CHECK(f); size_t n=fread(out,1,capacity,f);
    CHECK(!ferror(f) && feof(f)); CHECK(fclose(f)==0); return n;
}
static spurs_job_capture_status capture(spurs_job_snapshot* s,char* prefix)
{ return spurs_job_snapshot_capture(s,prefix,4096); }
int main(int argc,char** argv)
{
    CHECK(argc==3 && sizeof(void*)>=8); const char* mode=argv[1]; const char* dir=argv[2];
    CHECK(files(dir)==0);
#ifdef _WIN32
    SYSTEM_INFO info; GetSystemInfo(&info); page=info.dwPageSize;
    memory=(unsigned char*)VirtualAlloc(NULL,(SIZE_T)LIMIT,MEM_RESERVE,PAGE_NOACCESS); CHECK(memory);
#else
    long p=sysconf(_SC_PAGESIZE); CHECK(p>0); page=(size_t)p;
    memory=(unsigned char*)mmap(NULL,(size_t)LIMIT,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); CHECK(memory!=MAP_FAILED);
#endif
    CHECK(page<=65536 && (JOB%page)==0 && (IMAGE%page)==0);
    commit(JOB); commit(IMAGE); commit(AUX & ~(uint32_t)(page-1)); commit((uint32_t)(LIMIT-page));
    memory[JOB+5]=3; /* BE u64 image EA 0x0000000000030000. */
    memory[JOB+9]=1; /* Exactly16 synthetic image bytes. */
    for (unsigned i=0;i<16;++i) memory[IMAGE+i]=(unsigned char)(i+1);
    const unsigned char expected[4]={1,2,0,0};
    memcpy(memory+AUX,expected,4); memcpy(memory+UINT32_C(0xFFFFFFFC),expected,4);
    spurs_job_snapshot s={0};
    CHECK(spurs_job_snapshot_acquire(memory,LIMIT,JOB,128,&s)==SPURS_JOB_SNAPSHOT_OK);
    CHECK(s.guest_base==memory && s.guest_limit==LIMIT);
    environment("FOLKLORE_SPU_CAPTURE_DIR",dir);
    environment("FOLKLORE_SPU_CAPTURE_AUX_EA",NULL);
    char prefix[4096]={0}, data[4096];
    if (!strcmp(mode,"disabled")) {
        environment("FOLKLORE_SPU_CAPTURE_DIR",NULL);
        environment("FOLKLORE_SPU_CAPTURE_AUX_EA","0xFFFFFFFFFFFFFFFF");
        /* Invalid mapping provenance cannot be read while DIR disabled. */
        s.guest_base=(void*)(UINTPTR_MAX-1); s.guest_limit=LIMIT;
        CHECK(capture(&s,prefix)==SPURS_JOB_CAPTURE_DISABLED && !prefix[0]); CHECK(files(dir)==0);
    } else if (!strcmp(mode,"default") || !strcmp(mode,"valid") || !strcmp(mode,"readonly") || !strcmp(mode,"edge")) {
        int aux=strcmp(mode,"default")!=0;
        if (!strcmp(mode,"readonly")) {
#ifdef _WIN32
            DWORD old; CHECK(VirtualProtect(memory+(AUX & ~(uint32_t)(page-1)),page,PAGE_READONLY,&old));
#else
            CHECK(mprotect(memory+(AUX & ~(uint32_t)(page-1)),page,PROT_READ)==0);
#endif
        }
        if (aux) environment("FOLKLORE_SPU_CAPTURE_AUX_EA",!strcmp(mode,"edge") ? "0xFFFFFFFC" : "0x4501B900");
        CHECK(capture(&s,prefix)==SPURS_JOB_CAPTURE_WRITTEN);
        CHECK(files(dir)==(aux?4u:3u));
        size_t n=read_file(prefix,".json",data,sizeof data-1); data[n]=0;
        CHECK(strstr(data,aux ? "\"schema_version\": 2" : "\"schema_version\": 1"));
        if (aux) {
            CHECK(strstr(data,"\"auxiliary_size\": 4") && strstr(data,"separate checked copy; guest not quiesced"));
            CHECK(strstr(data,!strcmp(mode,"edge") ? "\"auxiliary_ea\": \"0xFFFFFFFC\"" : "\"auxiliary_ea\": \"0x4501B900\""));
            CHECK(strstr(data,".aux.bin\""));
            unsigned char bytes[5]; CHECK(read_file(prefix,".aux.bin",bytes,sizeof bytes)==4);
            CHECK(memcmp(bytes,expected,4)==0);
        } else CHECK(!strstr(data,"auxiliary_"));
    } else if (!strcmp(mode,"guard")) {
        unsigned char* target=memory+(AUX & ~(uint32_t)(page-1));
        environment("FOLKLORE_SPU_CAPTURE_AUX_EA","0x4501B900");
#ifdef _WIN32
        DWORD old; CHECK(VirtualProtect(target,page,PAGE_READWRITE|PAGE_GUARD,&old));
#else
        CHECK(mprotect(target,page,PROT_NONE)==0);
#endif
        CHECK(capture(&s,prefix)==SPURS_JOB_CAPTURE_ERROR && !prefix[0]); CHECK(files(dir)==0);
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION info; CHECK(VirtualQuery(target,&info,sizeof info)==sizeof info);
        CHECK(info.Protect & PAGE_GUARD); CHECK(VirtualProtect(target,page,PAGE_READWRITE,&old));
#else
        CHECK(mprotect(target,page,PROT_READ|PROT_WRITE)==0);
#endif
        CHECK(capture(&s,prefix)==SPURS_JOB_CAPTURE_WRITTEN && strstr(prefix,"-01")); CHECK(files(dir)==4);
    } else if (!strcmp(mode,"invalid")) {
        const char* bad[]={"0x","0x100000000","-1"," 0x4501B900","0x4501B900 ","0X4501B900","4501B900","0xGG","0x0","0xFFFF","0xFFFFFFFD","0x40000","0xFFFFFFFFFFFFFFFF"};
        for (unsigned i=0;i<SPURS_JOB_CAPTURE_MAX_COUNT;++i) {
            s.guest_base=memory; s.guest_limit=LIMIT;
            if (i<sizeof bad/sizeof bad[0]) environment("FOLKLORE_SPU_CAPTURE_AUX_EA",bad[i]);
            else if (i==13) {
                s.guest_limit=0x40000; environment("FOLKLORE_SPU_CAPTURE_AUX_EA","0x4501B900");
            } else if (i==14) {
                s.guest_base=(void*)(UINTPTR_MAX-1); environment("FOLKLORE_SPU_CAPTURE_AUX_EA","0x4501B900");
            } else {
                char crossing[32]; CHECK(snprintf(crossing,sizeof crossing,"0x%08X",(AUX & ~(uint32_t)(page-1))+(uint32_t)page-2)>0);
                environment("FOLKLORE_SPU_CAPTURE_AUX_EA",crossing);
            }
            CHECK(capture(&s,prefix)==SPURS_JOB_CAPTURE_ERROR && !prefix[0]); CHECK(files(dir)==0);
        }
        environment("FOLKLORE_SPU_CAPTURE_AUX_EA","0x4501B900");
        CHECK(capture(&s,prefix)==SPURS_JOB_CAPTURE_LIMIT && !prefix[0]); CHECK(files(dir)==0);
    } else if (!strcmp(mode,"aux-collision") || !strcmp(mode,"meta-collision")) {
        environment("FOLKLORE_SPU_CAPTURE_AUX_EA","0x4501B900");
        char reserved[4096];
        CHECK(snprintf(prefix,sizeof prefix,"%s/spurs-job-%08X-%016llX-00",dir,s.job_ea,(unsigned long long)s.fingerprint)>0);
        CHECK(snprintf(reserved,sizeof reserved,"%s%s",prefix,!strcmp(mode,"aux-collision") ? ".aux.bin" : ".json")>0);
        FILE* f=fopen(reserved,"wb"); CHECK(f); CHECK(fwrite("KEEP",1,4,f)==4); CHECK(fclose(f)==0);
        CHECK(capture(&s,prefix)==SPURS_JOB_CAPTURE_ERROR && !prefix[0]); CHECK(files(dir)==1);
        CHECK(exists(reserved)); f=fopen(reserved,"rb"); CHECK(f); CHECK(fread(data,1,5,f)==4 && !memcmp(data,"KEEP",4)); CHECK(fclose(f)==0);
        CHECK(capture(&s,prefix)==SPURS_JOB_CAPTURE_WRITTEN); CHECK(strstr(prefix,"-01")); CHECK(files(dir)==5);
    } else CHECK(0);
    spurs_job_snapshot_release(&s); CHECK(!s.guest_base && !s.guest_limit);
#ifdef _WIN32
    CHECK(VirtualFree(memory,0,MEM_RELEASE));
#else
    CHECK(munmap(memory,(size_t)LIMIT)==0);
#endif
    printf("PASS %s\n",mode); return 0;
}

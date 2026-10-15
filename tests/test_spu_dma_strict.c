/* Synthetic strict ordinary-MFC production-header fixture. Link the actual
 * runtime/HLE libraries, including real coherency and VM globals, no validation
 * or transfer substitutes. Fresh process per mode. Default main or define
 * SPU_DMA_STRICT_NATIVE and call test_spu_dma_strict(mode) before VM startup.
 * positive/readonly-get/writeonly-put expect0. Every other documented mode
 * requires real strict fatal86, never a host fault or FALSE_COMPLETION. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "spu_dma.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <signal.h>
#endif
extern uint32_t ppu_vm_size;
extern uint8_t g_vm_page_bitmap[65536/8];
#define VM_LIMIT UINT64_C(0x100000000)
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
static size_t os_page;
static int commit(uint32_t ea)
{
#ifdef _WIN32
    return VirtualAlloc(vm_base+ea,65536,MEM_COMMIT,PAGE_READWRITE)==vm_base+ea;
#else
    return mprotect(vm_base+ea,65536,PROT_READ|PROT_WRITE)==0;
#endif
}
static int protect(uint32_t ea,int read,int write,int guard)
{
#ifdef _WIN32
    DWORD old;
    DWORD access=write?PAGE_READWRITE:read?PAGE_READONLY:PAGE_NOACCESS;
    return VirtualProtect(vm_base+ea,os_page,access|(guard?PAGE_GUARD:0),&old)!=0;
#else
    return mprotect(vm_base+ea,os_page,guard?PROT_NONE:((read?PROT_READ:0)|(write?PROT_WRITE:0)))==0;
#endif
}
static int submit(mfc_engine* m,spu_context* s,uint32_t cmd,uint32_t ls,uint64_t ea,uint32_t size)
{
    s->mfc_lsa=ls;s->mfc_eah=(uint32_t)(ea>>32);s->mfc_eal=(uint32_t)ea;
    s->mfc_size=size;s->mfc_tag=2;
    return mfc_submit(m,s,cmd);
}
int test_spu_dma_strict(const char* mode)
{
    CHECK(mode && sizeof(void*)>=8 && vm_base==NULL);
#ifdef _WIN32
    CHECK(_putenv_s("PS3_STRICT_BOOT","1")==0);
    SYSTEM_INFO info;GetSystemInfo(&info);os_page=info.dwPageSize;
    vm_base=(uint8_t*)VirtualAlloc(NULL,(SIZE_T)VM_LIMIT,MEM_RESERVE,PAGE_NOACCESS);CHECK(vm_base);
#else
    CHECK(setenv("PS3_STRICT_BOOT","1",1)==0);
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);os_page=(size_t)page;
    vm_base=(uint8_t*)mmap(NULL,(size_t)VM_LIMIT,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(vm_base!=MAP_FAILED);
#endif
    CHECK(os_page<=65536 && 65536%os_page==0);
    ppu_vm_size=0;
    CHECK(commit(0x10000) && commit(0x20000) && commit(0x30000) && commit(0xFFFF0000));
    memset(vm_base+0x20000,0x6D,65536);memset(vm_base+0x30000,0x4C,65536);
    spu_context* s=(spu_context*)calloc(1,sizeof *s);CHECK(s);spu_context_init(s,0);
    memset(s->ls,0xA5,SPU_LS_SIZE);
    mfc_engine m;mfc_engine_init(&m);
    if(!strcmp(mode,"positive")) {
        const uint32_t sizes[]={1,2,4,8,16,32,128,16384};
        const uint32_t commands[]={MFC_GET_CMD,MFC_GETB_CMD,MFC_GETF_CMD,MFC_PUT_CMD,MFC_PUTB_CMD,MFC_PUTF_CMD};
        for(unsigned c=0;c<6;++c)for(unsigned n=0;n<sizeof sizes/sizeof sizes[0];++n) {
            uint32_t size=sizes[n],offset=size<16?16-size:0;
            uint32_t ls=0x2000+offset,ea=0x20000+offset;
            memset(s->ls+ls,0xA5,size);memset(vm_base+ea,0x6D,size);
            CHECK(submit(&m,s,commands[c],ls,ea,size)==0 && (m.tag_completed&4));
            if(mfc_is_get(commands[c]))CHECK(!memcmp(s->ls+ls,vm_base+ea,size));
            else CHECK(vm_base[ea]==0xA5 && vm_base[ea+size-1]==0xA5);
        }
        /* Both LS endpoints and final legal32-bit guest span. */
        CHECK(submit(&m,s,MFC_GET_CMD,0,0x20000,16)==0);
        CHECK(submit(&m,s,MFC_GET_CMD,0x3FFF0,UINT64_C(0xFFFFFFF0),16)==0);
        CHECK(submit(&m,s,MFC_PUT_CMD,0x3FFFF,UINT64_C(0xFFFFFFFF),1)==0);
        puts("PASS valid ordinary GET/PUT/barrier/fence sizes, LS/EA endpoints");return 0;
    }
    if(!strcmp(mode,"readonly-get")) {
        CHECK(protect(0x20000,1,0,0));
        CHECK(submit(&m,s,MFC_GET_CMD,0x1000,0x20000,16)==0 && s->ls[0x1000]==0x6D);
        puts("PASS GET read-only source");return 0;
    }
    if(!strcmp(mode,"writeonly-put")) {
#ifdef _WIN32
        /* Windows has no PAGE_WRITEONLY; exercise its writable allowlist. */
        CHECK(protect(0x20000,1,1,0));
#else
        CHECK(protect(0x20000,0,1,0));
#endif
        CHECK(submit(&m,s,MFC_PUT_CMD,0x1000,0x20000,16)==0);
        CHECK(protect(0x20000,1,1,0) && vm_base[0x20000]==0xA5);
        puts("PASS PUT write-permitted destination");return 0;
    }
    uint32_t cmd=MFC_GET_CMD,ls=0x1000,size=16;uint64_t ea=0x20000;
    if(!strcmp(mode,"high-ea")) ea|=UINT64_C(1)<<32;
    else if(!strcmp(mode,"guest-limit")) ppu_vm_size=0x20008;
    else if(!strcmp(mode,"base-overflow")) vm_base=(uint8_t*)(UINTPTR_MAX-0x10000);
    else if(!strcmp(mode,"ls-overrun")) {ls=0x3FFF0;size=32;}
    else if(!strcmp(mode,"ls-high")) ls=0x40000;
    else if(!strcmp(mode,"size3")) size=3;
    else if(!strcmp(mode,"size0")) size=0;
    else if(!strcmp(mode,"size-big")) size=0x4010;
    else if(!strcmp(mode,"misalign-small")) {size=4;ls+=2;ea+=2;}
    else if(!strcmp(mode,"misalign-large")) {ls+=4;ea+=4;}
    else if(!strcmp(mode,"guest-wrap")) {ea=UINT64_C(0xFFFFFFF0);size=32;}
    else if(!strcmp(mode,"protected-low")) ea=0;
    else if(!strcmp(mode,"unmapped")) ea=0x60000;
    else if(!strcmp(mode,"guard")) CHECK(protect(0x20000,1,1,1));
    else if(!strcmp(mode,"ls-protected")) {
        CHECK(commit(0x80000));s->ls=vm_base+0x80000;ls=0;
        CHECK(protect(0x80000,0,0,0));
    }
    else if(!strcmp(mode,"readonly-put")) {cmd=MFC_PUT_CMD;CHECK(protect(0x20000,1,0,0));}
    else if(!strcmp(mode,"stale-get") || !strcmp(mode,"stale-put")) {
        cmd=!strcmp(mode,"stale-put")?MFC_PUT_CMD:MFC_GET_CMD;
        CHECK(submit(&m,s,cmd,ls,ea,size)==0);
        g_vm_page_bitmap[2>>3]|=(uint8_t)(1u<<(2&7)); /* Deliberately stale compatibility cache. */
        CHECK(protect(0x20000,0,0,0));
    } else if(!strcmp(mode,"hole-get") || !strcmp(mode,"hole-put")) {
        cmd=!strcmp(mode,"hole-put")?MFC_PUT_CMD:MFC_GET_CMD;
        CHECK(protect(0x20000+(uint32_t)os_page,0,0,0));
        ea=0x20000+os_page-16;size=32;
    } else if(!strcmp(mode,"middle-get") || !strcmp(mode,"middle-put")) {
        if(os_page>4096) {fprintf(stderr,"UNSUPPORTED_TOPOLOGY requires OS pages<=4096\n");return 2;}
        cmd=!strcmp(mode,"middle-put")?MFC_PUT_CMD:MFC_GET_CMD;
        CHECK(protect(0x20000+(uint32_t)os_page,0,0,0));size=16384;
    } else if(!strcmp(mode,"list")) cmd=MFC_GETL_CMD;
    else {fprintf(stderr,"unknown mode %s\n",mode);return 2;}
    fprintf(stderr,"EXPECT_STRICT_REJECTION mode=%s before-copy-and-tag-completion\n",mode);
    int result=submit(&m,s,cmd,ls,ea,size);
    fprintf(stderr,"FALSE_COMPLETION result=%d tag=0x%X\n",result,m.tag_completed);
    return 91;
}
#ifndef SPU_DMA_STRICT_NATIVE
int main(int argc,char** argv) {return argc==2?test_spu_dma_strict(argv[1]):2;}
#endif

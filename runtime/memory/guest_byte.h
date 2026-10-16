/* Checked single-byte HLE parameters. Caller keeps the mapping stable.
 * No implicit commit, raw pointer dereference or host-permission override. */
#ifndef PS3RECOMP_GUEST_BYTE_H
#define PS3RECOMP_GUEST_BYTE_H
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>
#endif
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
static int ps3_guest_byte_address(uint64_t ea,uintptr_t *address)
{
 uint64_t limit=ppu_vm_size?ppu_vm_size:0x100000000ull;
 if(ea<0x10000ull||ea>=limit||ea>UINT32_MAX||ea>UINTPTR_MAX-(uintptr_t)vm_base)return 0;
 *address=(uintptr_t)vm_base+(uintptr_t)ea;return *address!=UINTPTR_MAX;
}
static int ps3_guest_byte_permission(uintptr_t address,int writing)
{
#ifdef _WIN32
 MEMORY_BASIC_INFORMATION m;
 if(!VirtualQuery((void*)address,&m,sizeof m)||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return 0;
 DWORD p=m.Protect&0xff;
 if(writing)return p==PAGE_READWRITE||p==PAGE_WRITECOPY||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;
 return p==PAGE_READONLY||p==PAGE_READWRITE||p==PAGE_WRITECOPY||p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;
#elif defined(__linux__)
 FILE*f=fopen("/proc/self/maps","r");if(!f)return 0;
 char line[1024];int valid=0;
 while(fgets(line,sizeof line,f)){unsigned long long first,last;char mode[5]={0};
  if(sscanf(line,"%llx-%llx %4s",&first,&last,mode)==3&&first<=address&&address<last){valid=writing?mode[1]=='w':mode[0]=='r';break;}}
 return fclose(f)==0&&valid;
#else
 (void)address;(void)writing;return 0;
#endif
}
static int ps3_guest_byte_writable(uint64_t ea)
{uintptr_t a;return ps3_guest_byte_address(ea,&a)&&ps3_guest_byte_permission(a,1);}
static int ps3_guest_byte_copy(uint64_t ea,uint8_t*value,int writing)
{
 uintptr_t a;if(!value||!ps3_guest_byte_address(ea,&a))return 0;
#if !defined(__linux__) || defined(_WIN32)
 if(!ps3_guest_byte_permission(a,writing))return 0;
#endif
#ifdef _WIN32
 SIZE_T n=0;
 return (writing?WriteProcessMemory(GetCurrentProcess(),(void*)a,value,1,&n):
                 ReadProcessMemory(GetCurrentProcess(),(void*)a,value,1,&n))&&n==1;
#elif defined(__linux__)
 /* The kernel checks the current mapping and requested read/write permission
  * during process_vm_*v. A /proc/self/maps scan for every byte is redundant
  * here and cannot prevent a concurrent permission change. Keep the separate
  * permission query for callers that only preflight, without copying. */
 struct iovec local={value,1},remote={(void*)a,1};ssize_t n;
 do{n=writing?process_vm_writev(getpid(),&local,1,&remote,1,0):process_vm_readv(getpid(),&local,1,&remote,1,0);}while(n<0&&errno==EINTR);
 return n==1;
#else
 return 0;
#endif
}
#endif

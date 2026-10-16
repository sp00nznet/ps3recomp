/* Checked multi-byte guest write preflight. Caller keeps the mapping stable;
 * later actual writes remain kernel-checked. No caching, implicit commit,
 * raw pointer dereference or host-permission override. */
#ifndef PS3RECOMP_GUEST_SPAN_H
#define PS3RECOMP_GUEST_SPAN_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "guest_byte.h"
/* Same guest rules as ps3_guest_byte_address, applied to [ea, ea+bytes). */
static int ps3_guest_span_address(uint64_t ea,size_t bytes,uintptr_t *first,uintptr_t *last)
{
 uint64_t limit=ppu_vm_size?ppu_vm_size:0x100000000ull;uintptr_t base=(uintptr_t)vm_base;
 if(!vm_base||!bytes||ea<0x10000ull||ea>UINT32_MAX||ea>=limit)return 0;
 if((uint64_t)bytes-1>limit-1-ea)return 0;
 if(ea>UINTPTR_MAX-base)return 0;
 *first=base+(uintptr_t)ea;
 if((uint64_t)(bytes-1)>(uint64_t)(UINTPTR_MAX-*first))return 0;
 *last=*first+(uintptr_t)(bytes-1);return *last!=UINTPTR_MAX;
}
#if defined(__linux__) && !defined(_WIN32)
/* One /proc/self/maps pass: the span must be covered by contiguous mappings
 * that are all writable. Other permission bits may differ between them. */
static int ps3_guest_span_scan(uintptr_t first,uintptr_t last)
{
 FILE*f=fopen("/proc/self/maps","r");if(!f)return 0;
 char line[512];int valid=0,start=1;unsigned long long cursor=first,end=last;
 while(fgets(line,sizeof line,f)){int whole=strchr(line,'\n')!=NULL;
  if(start){unsigned long long lo,hi;char mode[5]={0};
   if(sscanf(line,"%llx-%llx %4s",&lo,&hi,mode)==3&&hi>cursor){
    if(lo>cursor||mode[1]!='w')break;
    if(hi-1>=end){valid=1;break;}
    cursor=hi;}}
  start=whole;}
 return fclose(f)==0&&valid;
}
#endif
static int ps3_guest_span_writable(uint64_t ea,size_t bytes)
{
 uintptr_t first,last;if(!ps3_guest_span_address(ea,bytes,&first,&last))return 0;
#if defined(__linux__) && !defined(_WIN32)
 return ps3_guest_span_scan(first,last);
#else
 for(size_t i=0;i<bytes;i++)if(!ps3_guest_byte_writable(ea+i))return 0;
 return 1;
#endif
}
#endif

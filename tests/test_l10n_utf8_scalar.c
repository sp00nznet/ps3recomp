#include "cellL10n.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t*vm_base;
uint32_t ppu_vm_size;
#define T(x) do{if(!(x)){fprintf(stderr,"L10N_FAIL line=%d\n",__LINE__);return 1;}}while(0)
static unsigned utf8(uint32_t cp,uint8_t*b)
{
 if(cp<0x80){b[0]=cp;return 1;}
 if(cp<0x800){b[0]=0xC0|(cp>>6);b[1]=0x80|(cp&63);return 2;}
 if(cp<0x10000){b[0]=0xE0|(cp>>12);b[1]=0x80|((cp>>6)&63);b[2]=0x80|(cp&63);return 3;}
 b[0]=0xF0|(cp>>18);b[1]=0x80|((cp>>12)&63);b[2]=0x80|((cp>>6)&63);b[3]=0x80|(cp&63);return 4;
}
static int convert(void){return UTF8toUTF16((const u8*)(uintptr_t)0x10000,(u16*)(uintptr_t)0x11000,(u32*)(uintptr_t)0x12000);}
int main(int argc,char**argv)
{
 vm_base=calloc(1,0x20000);ppu_vm_size=0x20000;T(vm_base);
 if(argc==2&&!strcmp(argv[1],"bad-address")){
#ifdef _WIN32
  _putenv_s("PS3_STRICT_BOOT","1");
#else
  setenv("PS3_STRICT_BOOT","1",1);
#endif
  UTF8toUTF16((const u8*)(uintptr_t)0x20000,(u16*)(uintptr_t)0x11000,(u32*)(uintptr_t)0x12000);return 9;
 }
 uint32_t points[]={0,0x41,0x7F,0x80,0x7FF,0x800,0xD7FF,0xE000,0xFFFF,0x10000,0x1F600,0x10FFFF};
 for(unsigned i=0;i<sizeof points/sizeof*points;i++){
  uint32_t cp=points[i];unsigned n=utf8(cp,vm_base+0x10000);
  memset(vm_base+0x11000,0xA5,8);memset(vm_base+0x12000,0xA5,8);
  T(convert()==(int)n);
  unsigned units=cp>0xFFFF?2:1;
  T(vm_base[0x12000]==0&&vm_base[0x12001]==0&&vm_base[0x12002]==0&&vm_base[0x12003]==units);
  uint16_t high=cp>0xFFFF?0xD800+((cp-0x10000)>>10):(uint16_t)cp;
  T(vm_base[0x11000]==(high>>8)&&vm_base[0x11001]==(high&255));
  if(units==2){uint16_t low=0xDC00+((cp-0x10000)&1023);T(vm_base[0x11002]==(low>>8)&&vm_base[0x11003]==(low&255));}
  for(unsigned j=units*2;j<8;j++)T(vm_base[0x11000+j]==0xA5);
  for(unsigned j=4;j<8;j++)T(vm_base[0x12000+j]==0xA5);
 }
 const uint8_t bad[][4]={{0x80},{0xC0,0x80},{0xC1,0xBF},{0xE0,0x80,0x80},{0xED,0xA0,0x80},{0xF0,0x80,0x80,0x80},{0xF4,0x90,0x80,0x80},{0xF5,0x80,0x80,0x80},{0xFF},{0xE2,0x41,0xAC}};
 for(unsigned i=0;i<sizeof bad/sizeof*bad;i++){
  memcpy(vm_base+0x10000,bad[i],4);memset(vm_base+0x11000,0xA5,8);memset(vm_base+0x12000,0xA5,8);
  T(convert()==0);for(unsigned j=0;j<8;j++)T(vm_base[0x11000+j]==0xA5&&vm_base[0x12000+j]==0xA5);
 }
 vm_base[0x1FFFF]='Z';T(UTF8toUTF16((const u8*)(uintptr_t)0x1FFFF,(u16*)(uintptr_t)0x11000,(u32*)(uintptr_t)0x12000)==1);
 T(vm_base[0x11000]==0&&vm_base[0x11001]=='Z');
 free(vm_base);puts("L10N_SCALAR_PASS");return 0;
}

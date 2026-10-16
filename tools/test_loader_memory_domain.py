#!/usr/bin/env python3
"""Exercise the actual loader syscall switch bodies and real memory allocator."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
def main():
 if os.name=='nt':print('SKIP: standalone loader regression needs POSIX compiler');return 0
 src=(root/'runtime/ppu/ppu_loader.cpp').read_text()
 body=src[src.index('    case 330: {'):src.index('    case 988:',src.index('    case 330: {'))]
 pre='''#include "sys_memory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
uint8_t* vm_base; uint32_t ppu_vm_size; uint32_t ppu_hle_inject_base=0x20000000u;
}
uint32_t vm_read32(uint64_t a){auto p=vm_base+a;return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
void vm_write32(uint64_t a,uint32_t x){auto p=vm_base+a;p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x;}
uint32_t ps3_mmapper_block_size(uint32_t id){return id==1?0x100000u:0x200000u;}
void actual_loader_cases(ppu_context* ctx){switch(ctx->gpr[11]){
'''
 post='''default:std::abort();}}
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL loader line%d: %s\\n",__LINE__,#x);return 1;}}while(0)
int main(){CHECK(vm_init()==CELL_OK);lv2_syscall_table tbl{};sys_memory_init(&tbl);ppu_context c{};
c.gpr[11]=330;c.gpr[3]=0x100000;c.gpr[5]=0x100000;c.gpr[6]=0x10000;actual_loader_cases(&c);CHECK(c.gpr[3]==0);uint32_t base=vm_read32(0x10000);CHECK(base>=0x40000000u);
c={};c.gpr[11]=337;c.gpr[3]=0x10000000;c.gpr[4]=1;c.gpr[6]=0x10000;actual_loader_cases(&c);CHECK(c.gpr[3]!=0);
c.gpr[3]=base;actual_loader_cases(&c);CHECK(c.gpr[3]==0&&vm_read32(0x10000)==base);
c.gpr[3]=base;actual_loader_cases(&c);CHECK(c.gpr[3]!=0);
c={};c.gpr[11]=330;c.gpr[3]=0x100000;c.gpr[5]=0x100000;c.gpr[6]=0x10000;actual_loader_cases(&c);CHECK(c.gpr[3]==0);uint32_t second=vm_read32(0x10000);CHECK(second>=base+0x100000);
c={};c.gpr[11]=337;c.gpr[3]=second;c.gpr[4]=1;c.gpr[6]=0x10000;actual_loader_cases(&c);CHECK(c.gpr[3]==0&&vm_read32(0x10000)==second);
vm_shutdown();std::puts("PASS loader330/337 failure recovery, reservation bounds and independent cursors");}
'''
 with tempfile.TemporaryDirectory(prefix='ps3-loader-memory-') as tmp:
  tmp=Path(tmp);(tmp/'fixture.cpp').write_text(pre+body+post)
  subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG','-c',str(root/'runtime/syscalls/sys_memory.c'),'-o',str(tmp/'memory.o')],check=True,timeout=30)
  subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-UNDEBUG','-I'+str(root/'runtime/syscalls'),str(tmp/'fixture.cpp'),str(tmp/'memory.o'),'-pthread','-o',str(tmp/'fixture')],check=True,timeout=30)
  subprocess.run([str(tmp/'fixture')],check=True,timeout=10)
 return 0
if __name__=='__main__':raise SystemExit(main())

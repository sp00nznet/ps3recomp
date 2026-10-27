// Real directory stream and guest-memory ABI, without game data or a GPU.
#include "../ppu_fs.cpp"
#include <cassert>
#include <set>
#include <string>
extern "C" { uint8_t* vm_base = nullptr; uint32_t ppu_vm_size = 65536; }
extern "C" void vm_write32(uint64_t a, uint32_t v) {
    assert(a + 4 <= ppu_vm_size);
    for (int i=0;i<4;i++) vm_base[a+i]=uint8_t(v >> (24-i*8));
}
extern "C" void vm_write64(uint64_t a, uint64_t v) {
    vm_write32(a, uint32_t(v>>32)); vm_write32(a+4, uint32_t(v));
}
static uint32_t read32(uint32_t a) {
    return uint32_t(vm_base[a])<<24 | uint32_t(vm_base[a+1])<<16 |
           uint32_t(vm_base[a+2])<<8 | vm_base[a+3];
}
static ppu_context call(int fd, uint32_t size, uint32_t entries=0x100, uint32_t count=0x80) {
    ppu_context c={}; c.gpr[3]=fd; c.gpr[4]=entries; c.gpr[5]=size; c.gpr[6]=count;
    cellFsGetDirectoryEntries(&c); return c;
}
int main(int argc, char** argv) {
    assert(argc==2); uint8_t memory[65536]={}; vm_base=memory;
    DIR* d=opendir(argv[1]); assert(d);
    int fd=fd_alloc_dir(d); assert(fd>=3);
    snprintf(g_dir_path[fd],sizeof g_dir_path[fd],"%s",argv[1]);
    assert(uint32_t(call(fd,312,0).gpr[3])==0x8001000Du);
    assert(uint32_t(call(fd,312,0x100,0).gpr[3])==0x8001000Du);
    assert(uint32_t(call(999,312).gpr[3])==0x8001002Au);
    assert(call(fd,311).gpr[3]==0 && read32(0x80)==0);
    std::set<std::string> names;
    for (;;) {
        memset(memory+0x100,0xA5,625);
        auto c=call(fd,625); assert(c.gpr[3]==0);
        uint32_t n=read32(0x80); assert(n<=2);
        assert(memory[0x100+624]==0xA5); // incomplete trailing entry untouched
        if(!n) break;
        for(uint32_t i=0;i<n;i++) {
            uint32_t ea=0x100+i*312;
            std::string name(reinterpret_cast<char*>(memory+ea+54));
            assert(names.insert(name).second);
            assert(memory[ea+53]==name.size());
            if(name=="payload") {
                assert(memory[ea+52]==2);
                assert((read32(ea)&0xF000)==0x8000);
                assert(read32(ea+36)==0 && read32(ea+40)==7);
            } else if(name=="folder") {
                assert(memory[ea+52]==1);
                assert((read32(ea)&0xF000)==0x4000);
            }
        }
    }
    assert(names.count("payload") && names.count("folder") && names.count(".hidden"));
    assert(call(fd,312).gpr[3]==0 && read32(0x80)==0); // stable EOF
    closedir(d); g_dirs[fd]=nullptr;
    assert(uint32_t(call(fd,312).gpr[3])==0x8001002Au);
    puts("PASS: PPU directory-entry ABI, batching, bounds, errors, and EOF");
}

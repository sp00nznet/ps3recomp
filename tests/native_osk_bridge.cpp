/* Loader seams for a synthetic guest; native OSK/sysutil/HLE remain real. */
#include <cstdint>
#include <cstring>
#include <cstdlib>
extern "C" {
extern uint8_t* vm_base;
uint32_t ppu_prof_resolve_host(void*){return 0;}
uint64_t vm_read64(uint64_t address){uint64_t value;std::memcpy(&value,vm_base+address,8);return __builtin_bswap64(value);}
void ps3_indirect_call(void*){std::abort();}
uint64_t ppu_guest_call(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t){std::abort();}
}
void vm_write64(uint64_t address,uint64_t value){value=__builtin_bswap64(value);std::memcpy(vm_base+address,&value,8);}

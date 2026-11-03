/* The new import must report unavailable presence publishing as an API error,
 * rather than unresolved HLE or fabricated successful PSN publication. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static void* handler;
void ppu_hle_register_all(void);
void ps3_hle_register(unsigned nid,const char* name,void* fn){(void)name;if(nid==0xBE81C71C)handler=fn;}
int main(void){
 ppu_hle_register_all();assert(handler);
 typedef int32_t(*fn)(const void*,uint32_t);fn set=(fn)handler;
 assert((uint32_t)set((void*)0xFEFEEB0,3)==0x8002a663);
 assert((uint32_t)set((void*)0xFEFEEB0,1)==0x8002a663);
 assert((uint32_t)set((void*)0xFEFEEB0,2)==0x8002a663);
 assert((uint32_t)set(0,3)==0x8002a665);
 assert((uint32_t)set((void*)0xFEFEEB0,0)==0x8002a665);
 assert((uint32_t)set((void*)0xFEFEEB0,4)==0x8002a665);
 puts("NP presence details: real import and explicit unavailable publication PASS");
}

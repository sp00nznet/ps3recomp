/* A local account has no purchased entitlements. Exercise the real import,
 * initialization guard, count query and preservation of unused output. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../libs/network/sceNp.h"
static void* handler;
void ppu_hle_register_all(void);
const char* np_psnr_online_id(void){return "PlayerRetail";}
void ps3_hle_register(unsigned nid,const char* name,void* fn){(void)name;if(nid==0xB66D1C46)handler=fn;}
int main(void){
 ppu_hle_register_all();assert(handler);
 typedef int32_t(*fn)(void*,uint32_t);fn list=(fn)handler;
 assert(list(0,0)==(int32_t)SCE_NP_ERROR_NOT_INITIALIZED);
 assert(sceNpInit(0,0)==0);
 assert(list(0,0)==0);
 unsigned char sentinel[32];for(unsigned i=0;i<32;i++)sentinel[i]=0xa5;
 assert(list(sentinel,1)==0);
 for(unsigned i=0;i<32;i++)assert(sentinel[i]==0xa5);
 assert(sceNpTerm()==0);
 assert(list(0,0)==(int32_t)SCE_NP_ERROR_NOT_INITIALIZED);
 puts("NP entitlements: real import, lifecycle and empty list PASS");
}

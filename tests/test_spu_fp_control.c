#include "spu_fp_control.h"
#include <stdlib.h>
#include <string.h>
#define T(x) do{if(!(x))return __LINE__;}while(0)
int main(int argc,char**argv)
{
 if(argc!=2)return 2;
#ifdef _WIN32
 _putenv_s("PS3_STRICT_BOOT","1");
#else
 setenv("PS3_STRICT_BOOT","1",1);
#endif
 spu_context*a=calloc(1,sizeof*a),*b=calloc(1,sizeof*b);T(a&&b);
 spu_context_init(a,3);spu_context_init(b,7);
 a->gpr[0]._u32[0]=0x1234;
 u128 value={{0}};value._u32[0]=0x400;
 if(!strcmp(argv[1],"rounding")){
  spu_fp_write_control(a,value);spu_fp_note_arithmetic(a,0x5678,1);return 9;
 }
 if(!strcmp(argv[1],"flags")){
  spu_fp_note_arithmetic(a,0x5678,0);spu_fp_read_control(a,0x567C);return 9;
 }
 if(strcmp(argv[1],"state"))return 2;
 value._u32[0]=0xFFFFFFFF;value._u32[1]=0x12345678;
 spu_fp_write_control(a,value);u128 got=spu_fp_read_control(a,0x5678);
 T(got._u32[0]==0xF07&&got._u32[1]==(0x12345678&0x3F07));
 got=spu_fp_read_control(b,0x5678);T(!got._u32[0]&&!got._u32[1]);
 spu_fp_note_arithmetic(a,0x5678,0);T(a->fp_status_untracked&&!b->fp_status_untracked);
 value=(u128){{0}};spu_fp_write_control(a,value);T(!a->fp_status_untracked);
 got=spu_fp_read_control(a,0x5678);T(!got._u32[0]);
 free(a);free(b);puts("SPU_FP_CONTROL_PASS");return 0;
}

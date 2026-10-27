/* Synthetic shadow activation/unsupported-translation contracts. */
#include "rsx_fp_decompiler.h"
#include "rsx_test_programs.h"
#include <stdio.h>
#include <string.h>
static int checks,failures;
#define CHECK(x,n) do{checks++;if(!(x)){failures++;fprintf(stderr,"FAIL %s\n",n);}}while(0)
int main(void){u8 uc[512]={0};char old[65536],now[65536];rsx_fp_shadow_desc d[16]={{0}};u32 nc=0;
 rsx_test_fp_instr(uc,RSX_TEST_FP_OPC(0x18)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_INSRC_TC0|RSX_TEST_FP_END,RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);
 CHECK(rsx_fp_decompile_buffered_ex(uc,16,64,0,old,sizeof old,&nc)>0,"old API valid");CHECK(rsx_fp_decompile_buffered_shadow_ex(uc,16,64,0,d,now,sizeof now,&nc)>0&&!strcmp(old,now),"disabled metadata preserves all HLSL bytes");
 for(u32 f=0;f<8;f++){rsx_fp_shadow_desc_from_texture(d,0xB0,2,0,1,0x01010000,f<<28,0x80000000);CHECK(d[0].enabled==(f>0&&f<7),"eight activation controls");}
 rsx_fp_shadow_desc_from_texture(d,0xB0,2,0,1,0x01010000,3u<<28,0x80000004);CHECK(!d[0].enabled,"alpha-kill bypass");rsx_fp_shadow_desc_from_texture(d,0xA5,2,0,1,0x01010000,3u<<28,0x80000000);CHECK(!d[0].enabled,"ordinary color bypass");rsx_fp_shadow_desc_from_texture(d,0xB0,2,0,1,0x01010000,3u<<28,0);CHECK(!d[0].enabled,"disabled unit bypass");
 const u32 bad[6][6]={{0xB1,2,0,1,0x01010000,3},{0xB3,2,0,1,0x01010000,3},{0xB0,2,1,1,0x01010000,3},{0xB0,1,0,1,0x01010000,3},{0xB0,2,0,2,0x01010000,3},{0xB0,2,0,1,0x01010000,8}};
 for(int i=0;i<6;i++){rsx_fp_shadow_desc_from_texture(d,bad[i][0],bad[i][1],bad[i][2],bad[i][3],bad[i][4],bad[i][5]<<28,0x80000000);CHECK(d[0].enabled==2,"unsupported metadata flagged");CHECK(rsx_fp_decompile_buffered_shadow_ex(uc,16,64,0,d,now,sizeof now,&nc)<0,"unsupported active translation rejected");}
 const u32 ops[5]={0x19,0x2F,0x31,0x33,0x34};rsx_fp_shadow_desc_from_texture(d,0xB0,2,0,1,0x01010000,3u<<28,0x80000000);
 for(int i=0;i<5;i++){rsx_test_fp_instr(uc,RSX_TEST_FP_OPC(ops[i])|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_INSRC_TC0|RSX_TEST_FP_END,RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);CHECK(rsx_fp_texture_units(uc,16)==1,"unsupported texture fetch still participates in activation");CHECK(rsx_fp_decompile_buffered_shadow_ex(uc,16,64,0,d,now,sizeof now,&nc)<0,"unsupported fetch rejected");}
 for(u32 u=0;u<16;u++){rsx_test_fp_instr(uc+u*16,RSX_TEST_FP_OPC(0x17)|RSX_TEST_FP_TEXU(u)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_INSRC_TC0|(u==15?RSX_TEST_FP_END:0),RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);rsx_fp_shadow_desc_from_texture(&d[u],0xB0,2,0,1,0x02020000,3u<<28,0x80000000);}
 CHECK(rsx_fp_texture_units(uc,256)==65535,"all sixteen unit mask");CHECK(rsx_fp_decompile_buffered_shadow_ex(uc,256,64,0,d,now,sizeof now,&nc)>0&&strstr(now,"_rsx_shadow_cmp15"),"bounded output supports sixteen comparison helpers");
 printf("shadow metadata: %d checks, %d failures\n",checks,failures);return failures?1:0;}

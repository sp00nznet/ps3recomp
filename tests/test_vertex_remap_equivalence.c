/* Standalone: clang -O2 -Iinclude -Ilibs/video tests/test_vertex_remap_equivalence.c
 * libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c -o remap-test.exe */
#include "../libs/video/rsx_vertex_compact.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
static unsigned checks;
#define CHECK(c) do {++checks;if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static u32 seed=0x916bd281u;
static u32 next(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static void compare(rsx_vertex_remap* map,const rsx_vertex_ref* input,u32 n)
{
    rsx_vertex_ref actual[514],expected[514];u32 occurrence[513],unique=0,result=UINT_MAX;
    memset(actual,0xa5,sizeof(actual));memcpy(actual,input,n*sizeof(*input));
    /* Deliberately simple first-use oracle, independent of hashing/ranges. */
    for(u32 i=0;i<n;++i){
        u32 j=0;for(;j<unique;++j)
            if(expected[j].vertex_id==input[i].vertex_id && expected[j].base_index==input[i].base_index)break;
        if(j==unique)expected[unique++]=input[i];
        occurrence[i]=j;
    }
    CHECK(rsx_vertex_remap_build(map,actual,n,&result));CHECK(result==unique);
    CHECK(!memcmp(actual,expected,unique*sizeof(*actual)));
    CHECK(actual[n].vertex_id==0xa5a5a5a5u && actual[n].base_index==0xa5a5a5a5u);
    for(u32 i=0;i<n;++i)CHECK(rsx_vertex_remap_index(map,i)==occurrence[i]);
}
int main(void)
{
    rsx_vertex_remap map={0};rsx_vertex_ref input[513];u32 out;
    CHECK(!rsx_vertex_remap_build(NULL,input,1,&out));
    CHECK(!rsx_vertex_remap_build(&map,NULL,1,&out));
    CHECK(!rsx_vertex_remap_build(&map,input,1,NULL));
    CHECK(rsx_vertex_remap_build(&map,NULL,0,&out) && out==0);
    for(u32 trial=0;trial<4000;++trial){
        u32 n=next()%514,base=next(),mode=trial%8;
        for(u32 i=0;i<n;++i){
            u32 id=mode==0?next()%64:mode==1?UINT_MAX-next()%64:
                   mode==2?next():mode==3?i:mode==4?7:
                   mode==5?(i%2?0:UINT_MAX):mode==6?(next()%64)+0x100000u:next()%1026;
            input[i]=(rsx_vertex_ref){id,mode==2||mode==6?next()%3:base};
        }
        if(trial%97==0)map.generation=UINT_MAX; /* exercise wrap while reusing slots */
        compare(&map,input,n);
    }
    /* Exact dense-range threshold, one beyond, base change at the final item,
     * raw indices which would alias after the fetcher's wrapping. */
    for(u32 n=15;n<=17;++n)for(u32 extra=0;extra<2;++extra){
        for(u32 i=0;i<n;++i)input[i]=(rsx_vertex_ref){100+i%3,UINT_MAX};
        input[n-1].vertex_id=100+2*n-1+extra;compare(&map,input,n);
        input[n-1].base_index=0;compare(&map,input,n);
        input[n-1]=(rsx_vertex_ref){0x100064u,UINT_MAX};compare(&map,input,n);
    }
    rsx_vertex_remap_destroy(&map);CHECK(!map.slots && !map.occurrence_to_unique);
    printf("vertex remap equivalence: %u checks passed\n",checks);return 0;
}

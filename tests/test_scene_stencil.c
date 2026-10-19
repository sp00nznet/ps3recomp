#include "../libs/video/rsx_scene_stencil.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    unsigned checks=0;
    for(u32 op=0x200;op<=0x207;op++)for(u32 mask=0;mask<256;mask+=255) {
        /* Independent exhaustive byte-domain truth table for this GL op.
         * All possible reference/stored pairs must pass to qualify a draw. */
        int universal=1;
        for(u32 stored=0;stored<256;stored++)for(u32 reference=0;reference<256;reference++){
            u32 a=reference&mask,b=stored&mask;
            int pass=op==0x201?a<b:op==0x202?a==b:op==0x203?a<=b:
                     op==0x204?a>b:op==0x205?a!=b:op==0x206?a>=b:op==0x207;
            if(!pass)universal=0;
        }
        assert(rsx_scene_stencil_face_unconditional(op,mask)==universal);checks++;
    }
    rsx_be_render_state s={0};
    assert(rsx_scene_stencil_unconditional(&s));checks++;
    s.stencil_enable=1;s.s_func=0x207;s.s_func_mask=255;
    s.s_zpass=0x1e01; /* REPLACE still has unconditional depth visibility. */
    assert(rsx_scene_stencil_unconditional(&s));checks++;
    s.stencil_two_sided=1;s.bs_func=0x202;
    assert(!rsx_scene_stencil_unconditional(&s));checks++;
    s.s_func_mask=0;
    assert(rsx_scene_stencil_unconditional(&s));checks++;
    s.bs_func=0x200;
    assert(!rsx_scene_stencil_unconditional(&s));checks++;
    assert(!rsx_scene_stencil_face_unconditional(0xffff,0));checks++;
    printf("PASS stencil qualification: %u checks including exhaustive byte comparisons\n",checks);
    return 0;
}

/* Reuse the upstream synthetic FIFO/geometry builder, with a distinct WPOS FP.
 * Six sequential frames exercise cache separation and returning to old state.
 * The external runner independently verifies every saved RGB pixel. */
#define main unused_upstream_host_main
#include "../runtime/host/host_posix.c"
#undef main
#include "folklore_wpos_program.h"
#include <assert.h>
int main(int argc,char** argv)
{
    static const u32 windows[]={0x12D0,0x2D0,0x102D0,0x112D0,0x12CF,0x12D0};
    vm_base=host_vm_alloc();assert(vm_base);
    assert(host_backend_init(1280,720,"Synthetic WPOS regression")==0);
    assert(HOST_BACKEND_GUEST_SHADERS);
    u32 ctx=host_alloc(4,4);
    cellGcmSetupContext(ctx,CMD_SIZE,IO_SIZE,IO_ADDR,host_alloc,host_w32);
    cellGcmSetDisplayBuffer(0,0,5120,1280,720);
    if (argc==2 && !strcmp(argv[1],"bad-register")) {
        emit(NV4097_SET_SHADER_WINDOW,0x2000u);
        guest_w32(CTRL_ADDR,g_fifo_len);cellGcm_rsx_process_fifo();
        return 99; /* must stop86 before dispatch stores invalid state */
    }
    upload_shader_triangle();folklore_wpos_program(vm_base+IO_ADDR+FP_OFFSET);
    for (unsigned i=0;i<sizeof(windows)/sizeof(windows[0]);i++) {
        emit(NV4097_SET_SHADER_WINDOW,windows[i]);submit_frame(3);
        cellGcmSetFlipCommand(0);cellGcmTickVBlank();cellGcmTickFlip();
        cellGcm_take_flip_pending_synced();host_backend_present();
        assert(host_backend_guest_draws()==1);
        char name[64];snprintf(name,sizeof name,"frame-%u-%08X.ppm",i,windows[i]);
        const char* dump=getenv("PS3RECOMP_VK_DUMP");assert(dump && rename(dump,name)==0);
        printf("WPOS frame %u register 0x%08X pixel 0x%08X guest draws 1\n",i,windows[i],host_backend_center());
    }
    host_backend_shutdown();free(vm_base);return 0;
}

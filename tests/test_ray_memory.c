#include "../libs/video/rsx_ray_memory.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    const uint64_t gib = UINT64_C(1024)*1024*1024;
    const uint64_t trace1440 = UINT64_C(2560)*1440*2*40+128;
    /* 1440p's 281.25 MiB transport is permitted on small or large devices
     * when their actual limits/budgets allow it. GPU features are separate. */
    assert(rsx_ray_memory_fits(trace1440,gib,2*gib,1,gib,gib/2));
    assert(rsx_ray_memory_fits(trace1440,4*gib,16*gib,1,14*gib,4*gib));
    assert(!rsx_ray_memory_fits(trace1440,256*1024*1024,16*gib,0,0,0));
    assert(!rsx_ray_memory_fits(trace1440,4*gib,2*gib,1,gib,gib-1));
    assert(!rsx_ray_memory_fits(1,4*gib,2*gib,1,gib,gib+1));
    assert(!rsx_ray_memory_fits(1,4*gib,2*gib,1,0,0));
    assert(!rsx_ray_memory_fits(0,4*gib,2*gib,0,0,0));
    assert(!rsx_ray_memory_fits(1,0,2*gib,0,0,0));
    assert(!rsx_ray_memory_fits(3*gib,4*gib,2*gib,0,0,0));
    assert(rsx_ray_memory_fits(trace1440,gib,2*gib,0,0,0));
    assert(!rsx_ray_memory_fits(UINT64_MAX,UINT64_MAX,UINT64_MAX,1,UINT64_MAX,1));
    assert(rsx_ray_memory_fits(1,UINT64_MAX,UINT64_MAX,1,UINT64_MAX,UINT64_MAX-1));
    puts("PASS 12 PC ray allocation limit/budget checks");
    return 0;
}

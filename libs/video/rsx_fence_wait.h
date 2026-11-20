#ifndef PS3RECOMP_RSX_FENCE_WAIT_H
#define PS3RECOMP_RSX_FENCE_WAIT_H

#include <stdint.h>

static inline int rsx_fence_ready(
    int wait_succeeded, uint64_t completed, uint64_t required)
{
    return wait_succeeded &&
           completed != UINT64_MAX &&
           completed >= required;
}

#endif

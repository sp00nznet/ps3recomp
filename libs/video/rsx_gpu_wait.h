/* Waiting for a D3D12 fence value without giving up on a GPU that is only slow.
 *
 * The backend resets command allocators, rewrites upload rings and releases evicted objects once a wait returns, on
 * the assumption that the batch which used them has finished. A wait that gives up while the device is alive breaks
 * that assumption. The only wait worth giving up on is one on a removed device, whose fence never signals (a TDR);
 * giving up there is what keeps the vblank ticker, and so the whole emulation, from freezing.
 *
 * Pure logic with callbacks, so it is tested without a GPU (tests/test_rsx_gpu_wait.c). */
#ifndef RSX_GPU_WAIT_H
#define RSX_GPU_WAIT_H

typedef struct RsxGpuWaitOps {
    /* Wait up to two seconds for the fence value. Returns nonzero when it was reached, 0 on timeout. */
    int  (*wait_2s)(void* ctx);
    /* Nonzero when the device has been removed (GetDeviceRemovedReason != S_OK). */
    int  (*device_removed)(void* ctx);
    /* Called after each timeout, with the number of timeouts so far and the removed state. May be NULL. */
    void (*on_timeout)(void* ctx, unsigned timeouts, int removed);
} RsxGpuWaitOps;

/* Returns 1 when the fence value was reached; 0 only when the device was removed (nothing will ever signal). */
static inline int rsx_gpu_wait_alive(const RsxGpuWaitOps* ops, void* ctx)
{
    for (unsigned timeouts = 1;; timeouts++) {
        if (ops->wait_2s(ctx))
            return 1;
        int removed = ops->device_removed(ctx);
        if (ops->on_timeout)
            ops->on_timeout(ctx, timeouts, removed);
        if (removed)
            return 0;
    }
}

#endif

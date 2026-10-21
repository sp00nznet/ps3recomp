/* The bridge is upscaling only. ffx_vk.cpp stores this frame-interpolation
 * swapchain hook in every FfxInterface, but its definition lives in the
 * FrameInterpolationSwapchain sources, which are not built. The FSR 3.1
 * upscaler never calls it; anything that does is refused. */
#include <FidelityFX/host/backends/vk/ffx_vk.h>

FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const *config)
{
    (void)config;
    return FFX_ERROR_INCOMPLETE_INTERFACE;
}

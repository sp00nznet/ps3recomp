/* Builds the pinned FidelityFX ffx_vk.cpp unmodified and adds two queries
 * about its scratch layout. CreateBackendContextVK carves the scratch buffer
 * into arrays rounded only to 4 bytes, so its alignas(32) EffectContext array
 * is not 32-aligned for an aligned base; MSVC's unaligned moves hide that,
 * clang's aligned SSE stores fault. The bridge places the base so this
 * offset lands on 32 bytes, then checks the backend's real pointer. */
#include "backends/vk/ffx_vk.cpp"

/* Mirrors the "Map all of our pointers" sizes in CreateBackendContextVK. */
extern "C" size_t rsx_ffx_vk_effect_context_offset(size_t max_contexts)
{
    const size_t m = max_contexts;
    size_t offset = sizeof(BackendContext_VK);
    offset += FFX_ALIGN_UP(m * FFX_MAX_GPU_JOBS * sizeof(FfxGpuJobDescription), sizeof(uint32_t));
    offset += FFX_ALIGN_UP(((m * FFX_MAX_QUEUED_FRAMES * FFX_MAX_RESOURCE_COUNT * 2) + FFX_MAX_BINDLESS_DESCRIPTOR_COUNT) *
                               sizeof(BackendContext_VK::VkResourceView), sizeof(uint32_t));
    offset += FFX_ALIGN_UP(m * FFX_CONSTANT_BUFFER_RING_BUFFER_SIZE, sizeof(uint32_t));
    offset += FFX_ALIGN_UP(m * FFX_MAX_PASS_COUNT * sizeof(BackendContext_VK::PipelineLayout), sizeof(uint32_t));
    offset += FFX_ALIGN_UP(m * FFX_MAX_RESOURCE_COUNT * sizeof(BackendContext_VK::Resource), sizeof(uint32_t));
    return offset;
}

extern "C" size_t rsx_ffx_vk_effect_context_alignment(void)
{
    return alignof(BackendContext_VK::EffectContext);
}

/* The array the backend actually mapped (null before its first context). */
extern "C" const void *rsx_ffx_vk_effect_contexts(const void *scratch)
{
    return static_cast<const BackendContext_VK *>(scratch)->pEffectContexts;
}

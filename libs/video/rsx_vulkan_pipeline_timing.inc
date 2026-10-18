/* Opt-in evidence for first-use stutters, not a prewarm or an optimization.
 * Backend/render thread only. Default path performs no clock reads or I/O.
 * PS3RECOMP_VK_PIPELINE_TIMING=1 logs at most 256 >=2ms creates or failures.
 * host_us uses the monotonic performance counter for correlation with jobs. */
static int s_pipeline_timing = -1;
static unsigned s_pipeline_timing_lines;

static VkResult vk_create_graphics_timed(const char* kind, u32 id,
    VkDevice device, VkPipelineCache cache, u32 count,
    const VkGraphicsPipelineCreateInfo* infos,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines)
{
    if (s_pipeline_timing < 0) {
        const char* e = getenv("PS3RECOMP_VK_PIPELINE_TIMING");
        s_pipeline_timing = e && !strcmp(e, "1");
    }
    if (!s_pipeline_timing || s_pipeline_timing_lines >= 256)
        return pvkCreateGraphicsPipelines(device, cache, count, infos, allocator, pipelines);
    const Uint64 before = SDL_GetPerformanceCounter();
    const VkResult result = pvkCreateGraphicsPipelines(device, cache, count, infos, allocator, pipelines);
    const Uint64 after = SDL_GetPerformanceCounter();
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    if (frequency && after >= before) {
        const double elapsed_us = (double)(after-before) * 1000000.0 / (double)frequency;
        if (elapsed_us >= 2000.0 || result != VK_SUCCESS) {
            VK_LOG("pipeline-time kind=%s id=%u host_us=%.0f elapsed_us=%.0f result=%d cache=%d\n",
                kind, id, (double)before * 1000000.0 / (double)frequency,
                elapsed_us, (int)result, cache != VK_NULL_HANDLE);
            ++s_pipeline_timing_lines;
        }
    }
    return result;
}

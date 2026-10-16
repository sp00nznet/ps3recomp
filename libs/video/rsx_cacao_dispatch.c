/*
 * rsx_cacao_dispatch.c - transactional binding of the CACAO Vulkan dispatch.
 *
 * Never opens the Vulkan loader and never references exported Vulkan
 * symbols; only the caller-supplied vkGetInstanceProcAddr and the device
 * resolver it yields are invoked. Not thread safe: callers serialize
 * bind/reset against all CACAO use.
 */
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#undef RSX_CACAO_MAP_VULKAN

#include "rsx_cacao_dispatch.h"

#include <stddef.h>
#include <string.h>

rsx_cacao_dispatch rsx_cacao_vk;

/* Entries resolved through vkGetDeviceProcAddr. */
#define RSX_CACAO_DEVICE_FUNCS(X)          \
    X(vkAllocateDescriptorSets)            \
    X(vkAllocateMemory)                    \
    X(vkBindBufferMemory)                  \
    X(vkBindImageMemory)                   \
    X(vkCmdBindDescriptorSets)             \
    X(vkCmdBindPipeline)                   \
    X(vkCmdDispatch)                       \
    X(vkCmdPipelineBarrier)                \
    X(vkCmdResetQueryPool)                 \
    X(vkCmdWriteTimestamp)                 \
    X(vkCreateBuffer)                      \
    X(vkCreateComputePipelines)            \
    X(vkCreateDescriptorPool)              \
    X(vkCreateDescriptorSetLayout)         \
    X(vkCreateImage)                       \
    X(vkCreateImageView)                   \
    X(vkCreatePipelineLayout)              \
    X(vkCreateQueryPool)                   \
    X(vkCreateSampler)                     \
    X(vkCreateShaderModule)                \
    X(vkDestroyBuffer)                     \
    X(vkDestroyDescriptorPool)             \
    X(vkDestroyDescriptorSetLayout)        \
    X(vkDestroyImage)                      \
    X(vkDestroyImageView)                  \
    X(vkDestroyPipeline)                   \
    X(vkDestroyPipelineLayout)             \
    X(vkDestroyQueryPool)                  \
    X(vkDestroySampler)                    \
    X(vkDestroyShaderModule)               \
    X(vkFreeMemory)                        \
    X(vkGetBufferMemoryRequirements)       \
    X(vkGetImageMemoryRequirements)        \
    X(vkGetQueryPoolResults)               \
    X(vkMapMemory)                         \
    X(vkUnmapMemory)                       \
    X(vkUpdateDescriptorSets)

int rsx_cacao_dispatch_bind(VkInstance instance, VkDevice device,
                            PFN_vkGetInstanceProcAddr get_instance_proc)
{
    rsx_cacao_dispatch local;

    if (instance == NULL || device == NULL || get_instance_proc == NULL)
        return 0;

    memset(&local, 0, sizeof(local));

    local.vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        get_instance_proc(instance, "vkGetDeviceProcAddr");
    local.vkGetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
        get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties");
    if (local.vkGetDeviceProcAddr == NULL ||
        local.vkGetPhysicalDeviceMemoryProperties == NULL)
        return 0;

#define RSX_CACAO_LOAD(name)                                          \
    local.name = (PFN_##name)local.vkGetDeviceProcAddr(device, #name); \
    if (local.name == NULL)                                           \
        return 0;
    RSX_CACAO_DEVICE_FUNCS(RSX_CACAO_LOAD)
#undef RSX_CACAO_LOAD

    rsx_cacao_vk = local;
    return 1;
}

void rsx_cacao_dispatch_reset(void)
{
    memset(&rsx_cacao_vk, 0, sizeof(rsx_cacao_vk));
}

/*
 * rsx_cacao_dispatch.h - dynamically loaded Vulkan dispatch table for the
 * FidelityFX CACAO (MIT) Vulkan backend.
 *
 * Upstream CACAO is compiled with VK_NO_PROTOTYPES and this header included
 * first with RSX_CACAO_MAP_VULKAN defined, so every Vulkan call it makes is
 * routed through rsx_cacao_vk instead of the loader's exported symbols.
 *
 * Single-device and externally synchronized: bind/reset only while no live
 * CACAO context or recorded command still uses the previous dispatch.
 */
#ifndef RSX_CACAO_DISPATCH_H
#define RSX_CACAO_DISPATCH_H

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_cacao_dispatch {
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
    PFN_vkAllocateMemory vkAllocateMemory;
    PFN_vkBindBufferMemory vkBindBufferMemory;
    PFN_vkBindImageMemory vkBindImageMemory;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
    PFN_vkCmdBindPipeline vkCmdBindPipeline;
    PFN_vkCmdDispatch vkCmdDispatch;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
    PFN_vkCmdResetQueryPool vkCmdResetQueryPool;
    PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp;
    PFN_vkCreateBuffer vkCreateBuffer;
    PFN_vkCreateComputePipelines vkCreateComputePipelines;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
    PFN_vkCreateImage vkCreateImage;
    PFN_vkCreateImageView vkCreateImageView;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
    PFN_vkCreateQueryPool vkCreateQueryPool;
    PFN_vkCreateSampler vkCreateSampler;
    PFN_vkCreateShaderModule vkCreateShaderModule;
    PFN_vkDestroyBuffer vkDestroyBuffer;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
    PFN_vkDestroyImage vkDestroyImage;
    PFN_vkDestroyImageView vkDestroyImageView;
    PFN_vkDestroyPipeline vkDestroyPipeline;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
    PFN_vkDestroyQueryPool vkDestroyQueryPool;
    PFN_vkDestroySampler vkDestroySampler;
    PFN_vkDestroyShaderModule vkDestroyShaderModule;
    PFN_vkFreeMemory vkFreeMemory;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    PFN_vkGetQueryPoolResults vkGetQueryPoolResults;
    PFN_vkMapMemory vkMapMemory;
    PFN_vkUnmapMemory vkUnmapMemory;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
} rsx_cacao_dispatch;

extern rsx_cacao_dispatch rsx_cacao_vk;

/* Resolves every table entry into a local table and publishes it only when
 * all are non-null. vkGetDeviceProcAddr and
 * vkGetPhysicalDeviceMemoryProperties come from get_instance_proc; the rest
 * come from the obtained vkGetDeviceProcAddr. Returns 1 on success, 0 on
 * null arguments or any missing function, leaving rsx_cacao_vk untouched. */
int rsx_cacao_dispatch_bind(VkInstance instance, VkDevice device,
                            PFN_vkGetInstanceProcAddr get_instance_proc);

/* Clears rsx_cacao_vk to all-null. */
void rsx_cacao_dispatch_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RSX_CACAO_DISPATCH_H */

/* Opt-in aliasing for the upstream CACAO wrapper TU only. Kept outside the
 * main guard so the table above is always parsed before any alias exists. */
#if defined(RSX_CACAO_MAP_VULKAN) && !defined(RSX_CACAO_DISPATCH_MAPPED)
#define RSX_CACAO_DISPATCH_MAPPED
#define vkAllocateDescriptorSets rsx_cacao_vk.vkAllocateDescriptorSets
#define vkAllocateMemory rsx_cacao_vk.vkAllocateMemory
#define vkBindBufferMemory rsx_cacao_vk.vkBindBufferMemory
#define vkBindImageMemory rsx_cacao_vk.vkBindImageMemory
#define vkCmdBindDescriptorSets rsx_cacao_vk.vkCmdBindDescriptorSets
#define vkCmdBindPipeline rsx_cacao_vk.vkCmdBindPipeline
#define vkCmdDispatch rsx_cacao_vk.vkCmdDispatch
#define vkCmdPipelineBarrier rsx_cacao_vk.vkCmdPipelineBarrier
#define vkCmdResetQueryPool rsx_cacao_vk.vkCmdResetQueryPool
#define vkCmdWriteTimestamp rsx_cacao_vk.vkCmdWriteTimestamp
#define vkCreateBuffer rsx_cacao_vk.vkCreateBuffer
#define vkCreateComputePipelines rsx_cacao_vk.vkCreateComputePipelines
#define vkCreateDescriptorPool rsx_cacao_vk.vkCreateDescriptorPool
#define vkCreateDescriptorSetLayout rsx_cacao_vk.vkCreateDescriptorSetLayout
#define vkCreateImage rsx_cacao_vk.vkCreateImage
#define vkCreateImageView rsx_cacao_vk.vkCreateImageView
#define vkCreatePipelineLayout rsx_cacao_vk.vkCreatePipelineLayout
#define vkCreateQueryPool rsx_cacao_vk.vkCreateQueryPool
#define vkCreateSampler rsx_cacao_vk.vkCreateSampler
#define vkCreateShaderModule rsx_cacao_vk.vkCreateShaderModule
#define vkDestroyBuffer rsx_cacao_vk.vkDestroyBuffer
#define vkDestroyDescriptorPool rsx_cacao_vk.vkDestroyDescriptorPool
#define vkDestroyDescriptorSetLayout rsx_cacao_vk.vkDestroyDescriptorSetLayout
#define vkDestroyImage rsx_cacao_vk.vkDestroyImage
#define vkDestroyImageView rsx_cacao_vk.vkDestroyImageView
#define vkDestroyPipeline rsx_cacao_vk.vkDestroyPipeline
#define vkDestroyPipelineLayout rsx_cacao_vk.vkDestroyPipelineLayout
#define vkDestroyQueryPool rsx_cacao_vk.vkDestroyQueryPool
#define vkDestroySampler rsx_cacao_vk.vkDestroySampler
#define vkDestroyShaderModule rsx_cacao_vk.vkDestroyShaderModule
#define vkFreeMemory rsx_cacao_vk.vkFreeMemory
#define vkGetBufferMemoryRequirements rsx_cacao_vk.vkGetBufferMemoryRequirements
#define vkGetDeviceProcAddr rsx_cacao_vk.vkGetDeviceProcAddr
#define vkGetImageMemoryRequirements rsx_cacao_vk.vkGetImageMemoryRequirements
#define vkGetPhysicalDeviceMemoryProperties rsx_cacao_vk.vkGetPhysicalDeviceMemoryProperties
#define vkGetQueryPoolResults rsx_cacao_vk.vkGetQueryPoolResults
#define vkMapMemory rsx_cacao_vk.vkMapMemory
#define vkUnmapMemory rsx_cacao_vk.vkUnmapMemory
#define vkUpdateDescriptorSets rsx_cacao_vk.vkUpdateDescriptorSets
#endif /* RSX_CACAO_MAP_VULKAN */

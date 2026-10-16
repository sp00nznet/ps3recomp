#define VK_NO_PROTOTYPES
#include "rsx_cacao_dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static const char *missing;
static const VkInstance instance=(VkInstance)(uintptr_t)1;
static const VkDevice device=(VkDevice)(uintptr_t)2;
static VKAPI_ATTR void VKAPI_CALL stub(void) {}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice d,const char *name) {
    CHECK(d==device);
    if(missing && !strcmp(name,missing)) return NULL;
    return stub;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instance_proc(VkInstance i,const char *name) {
    CHECK(i==instance);
    if(missing && !strcmp(name,missing)) return NULL;
    if(!strcmp(name,"vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)device_proc;
    CHECK(!strcmp(name,"vkGetPhysicalDeviceMemoryProperties")); return stub;
}
int main(void) {
    const char *names[]={
        "vkAllocateDescriptorSets","vkAllocateMemory","vkBindBufferMemory","vkBindImageMemory",
        "vkCmdBindDescriptorSets","vkCmdBindPipeline","vkCmdDispatch","vkCmdPipelineBarrier",
        "vkCmdResetQueryPool","vkCmdWriteTimestamp","vkCreateBuffer","vkCreateComputePipelines",
        "vkCreateDescriptorPool","vkCreateDescriptorSetLayout","vkCreateImage","vkCreateImageView",
        "vkCreatePipelineLayout","vkCreateQueryPool","vkCreateSampler","vkCreateShaderModule",
        "vkDestroyBuffer","vkDestroyDescriptorPool","vkDestroyDescriptorSetLayout","vkDestroyImage",
        "vkDestroyImageView","vkDestroyPipeline","vkDestroyPipelineLayout","vkDestroyQueryPool",
        "vkDestroySampler","vkDestroyShaderModule","vkFreeMemory","vkGetBufferMemoryRequirements",
        "vkGetDeviceProcAddr","vkGetImageMemoryRequirements","vkGetPhysicalDeviceMemoryProperties",
        "vkGetQueryPoolResults","vkMapMemory","vkUnmapMemory","vkUpdateDescriptorSets"};
    rsx_cacao_dispatch zero={0},before;
    CHECK(!memcmp(&rsx_cacao_vk,&zero,sizeof(zero)));
    CHECK(!rsx_cacao_dispatch_bind(NULL,device,instance_proc));
    CHECK(!rsx_cacao_dispatch_bind(instance,NULL,instance_proc));
    CHECK(!rsx_cacao_dispatch_bind(instance,device,NULL));
    for(unsigned populated=0;populated<2;++populated) {
        if(populated) { missing=NULL;CHECK(rsx_cacao_dispatch_bind(instance,device,instance_proc)); }
        before=rsx_cacao_vk;
        for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i) {
            missing=names[i];CHECK(!rsx_cacao_dispatch_bind(instance,device,instance_proc));
            CHECK(!memcmp(&before,&rsx_cacao_vk,sizeof(before)));
        }
    }
    CHECK(rsx_cacao_vk.vkGetDeviceProcAddr==device_proc);
    CHECK((PFN_vkVoidFunction)rsx_cacao_vk.vkUpdateDescriptorSets==stub);
    rsx_cacao_dispatch_reset();CHECK(!memcmp(&rsx_cacao_vk,&zero,sizeof(zero)));
    puts("CACAO dispatch: all missing-entry and transactional checks passed");
    printf("%u checks passed\n",checks);return 0;
}

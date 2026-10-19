/* Borrow the game's Vulkan device. Loading/capability discovery is separate
 * from evaluation and presentation; only actual completed frames may claim SR. */
#if defined(_WIN32)
#include "rsx_dlss_loader.h"
static int vk_dlss_frame_initialize(void);
static void vk_dlss_frame_destroy(void);
static struct {
    rsx_dlss_loader loader;
    rsx_dlss_vk *context;
    rsx_dlss_extensions instance_extensions, device_extensions;
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceVulkan12Features f12;
    int device_ready;
} s_dlss;

static int vk_dlss_requested(void)
{
    const char *v=getenv("PS3RECOMP_DLSS");return v && !strcmp(v,"1");
}
static int vk_dlss_wide_env(const char *name,wchar_t *out,size_t count)
{
    const char *v=getenv(name);
    return v && *v && MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,v,-1,out,(int)count)>0;
}
static int vk_dlss_merge(const rsx_dlss_extensions *required,const char **names,u32 *count,u32 capacity)
{
    if(required->count>RSX_DLSS_MAX_EXTENSIONS)return 0;
    for(u32 i=0;i<required->count;i++){
        int found=0;
        for(u32 j=0;j<*count;j++)if(!strcmp(names[j],required->items[i].extensionName))found=1;
        if(!found){if(*count==capacity)return 0;names[(*count)++]=required->items[i].extensionName;}
    }
    return 1;
}
static int vk_dlss_instance_prepare(VkInstanceCreateInfo *ci,const char **names,u32 capacity,u32 api)
{
    if(!vk_dlss_requested())return 0;
    if(api<VK_API_VERSION_1_2){VK_LOG("DLSS initialization requires Vulkan1.2\n");return 0;}
    wchar_t bridge[32768],runtime[32768],logs[32768];char error[256];
    if(!vk_dlss_wide_env("PS3RECOMP_DLSS_BRIDGE",bridge,32768) ||
       !vk_dlss_wide_env("PS3RECOMP_DLSS_RUNTIME",runtime,32768) ||
       !vk_dlss_wide_env("PS3RECOMP_DLSS_LOG",logs,32768)){
        VK_LOG("DLSS initialization requires explicit bridge/runtime/log paths\n");return 0;
    }
    if(!rsx_dlss_loader_open(&s_dlss.loader,bridge,runtime,logs,getenv("PS3RECOMP_DLSS_PROJECT_UUID"),error,sizeof error)){
        VK_LOG("DLSS loader rejected: %s\n",error);return 0;
    }
    int32_t ngx=0;
    rsx_dlss_status status=s_dlss.loader.instance_extensions(&s_dlss.loader.identity,&s_dlss.instance_extensions,&ngx);
    PFN_vkEnumerateInstanceExtensionProperties enumerate=(PFN_vkEnumerateInstanceExtensionProperties)
        pvkGetInstanceProcAddr(VK_NULL_HANDLE,"vkEnumerateInstanceExtensionProperties");
    u32 count=0;VkExtensionProperties *available=NULL;int good=0;
    if(status==RSX_DLSS_OK && enumerate && enumerate(NULL,&count,NULL)==VK_SUCCESS && count && count<=4096){
        available=(VkExtensionProperties*)calloc(count,sizeof *available);
        if(available && enumerate(NULL,&count,available)==VK_SUCCESS){
            good=1;
            for(u32 i=0;i<s_dlss.instance_extensions.count;i++)
                if(!vk_advanced_has(available,count,s_dlss.instance_extensions.items[i].extensionName))good=0;
        }
    }
    free(available);
    u32 used=ci->enabledExtensionCount;
    if(good && used<=capacity){
        for(u32 i=0;i<used;i++)names[i]=ci->ppEnabledExtensionNames[i];
        good=vk_dlss_merge(&s_dlss.instance_extensions,names,&used,capacity);
    }else good=0;
    if(!good){VK_LOG("DLSS instance requirements unavailable status=%d ngx=0x%x\n",status,(unsigned)ngx);rsx_dlss_loader_close(&s_dlss.loader);return 0;}
    ci->enabledExtensionCount=used;ci->ppEnabledExtensionNames=names;
    VK_LOG("DLSS explicit bridge loaded; instance requirements=%u\n",s_dlss.instance_extensions.count);
    return 1;
}
static int vk_dlss_device_prepare(VkDeviceCreateInfo *ci,const char **names,u32 capacity)
{
    if(!s_dlss.loader.module)return 0;
    if(s_advanced.device_api<VK_API_VERSION_1_2){rsx_dlss_loader_close(&s_dlss.loader);return 0;}
    int32_t ngx=0;rsx_dlss_status status=s_dlss.loader.device_extensions(
        &s_dlss.loader.identity,s_vk.instance,s_vk.phys,&s_dlss.device_extensions,&ngx);
    if(status!=RSX_DLSS_OK || s_dlss.device_extensions.count>RSX_DLSS_MAX_EXTENSIONS)goto fail;
    for(u32 i=0;i<s_dlss.device_extensions.count;i++)
        if(!vk_has_device_extension(s_vk.phys,s_dlss.device_extensions.items[i].extensionName))goto fail;
    u32 count=ci->enabledExtensionCount;
    if(!vk_dlss_merge(&s_dlss.device_extensions,names,&count,capacity))goto fail;
    PFN_vkGetPhysicalDeviceFeatures2 query=(PFN_vkGetPhysicalDeviceFeatures2)
        pvkGetInstanceProcAddr(s_vk.instance,"vkGetPhysicalDeviceFeatures2");
    if(!query)goto fail;
    s_dlss.f11=(VkPhysicalDeviceVulkan11Features){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    s_dlss.f12=(VkPhysicalDeviceVulkan12Features){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    s_dlss.f11.pNext=&s_dlss.f12;
    VkPhysicalDeviceFeatures2 supported={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&s_dlss.f11};
    query(s_vk.phys,&supported);
    s_dlss.f12.pNext=(void*)ci->pNext;ci->pNext=&s_dlss.f11;
    ci->enabledExtensionCount=count;ci->ppEnabledExtensionNames=names;
    return 1;
fail:
    VK_LOG("DLSS device requirements unavailable status=%d ngx=0x%x\n",status,(unsigned)ngx);
    rsx_dlss_loader_close(&s_dlss.loader);return 0;
}
static void vk_dlss_device_created(void)
{
    if(!s_dlss.loader.module || !s_dlss.device_ready)return;
    rsx_dlss_device device={s_vk.instance,s_vk.phys,s_vk.device,pvkGetInstanceProcAddr,pvkGetDeviceProcAddr};
    rsx_dlss_report report={0};
    rsx_dlss_status status=s_dlss.loader.create(&s_dlss.loader.identity,&device,&s_dlss.context,&report);
    VK_LOG("DLSS game-device context status=%d available=%u support=0x%x ngx=0x%x; evaluatedFrames=0\n",
        status,report.available,report.support_flags,(unsigned)report.ngx_result);
    if(status!=RSX_DLSS_OK){s_dlss.context=NULL;s_dlss.device_ready=0;}
    else vk_dlss_frame_initialize();
}
static void vk_dlss_device_selected(int enabled){s_dlss.device_ready=enabled;}
static void vk_dlss_shutdown(void)
{
    /* Caller drains the borrowed device before this function. */
    vk_dlss_frame_destroy();
    if(s_dlss.context)s_dlss.loader.destroy(s_dlss.context);
    rsx_dlss_loader_close(&s_dlss.loader);memset(&s_dlss,0,sizeof s_dlss);
}
#else
static int vk_dlss_requested(void){return 0;}
static int vk_dlss_instance_prepare(VkInstanceCreateInfo *c,const char **n,u32 z,u32 a){(void)c;(void)n;(void)z;(void)a;return 0;}
static int vk_dlss_device_prepare(VkDeviceCreateInfo *c,const char **n,u32 z){(void)c;(void)n;(void)z;return 0;}
static void vk_dlss_device_created(void){}
static void vk_dlss_device_selected(int enabled){(void)enabled;}
static void vk_dlss_shutdown(void){}
#endif

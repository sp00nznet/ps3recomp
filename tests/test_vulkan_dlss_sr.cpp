/* Private, offline NVIDIA DLSS Super Resolution Vulkan diagnostic.
 * SDK reference: NVIDIA/DLSS 9981c6350dc58c0ce21f024fd65e31e851c31689.
 * Build with the four NGX Vulkan API headers and MSVC /MD + nvsdk_ngx_d.lib.
 * No Frame Generation, Ray Reconstruction, game resources, or shared devices.
 * --probe LOG_DIRECTORY RUNTIME_DIRECTORY PROJECT_UUID
 * --static PREFIX LOG_DIRECTORY RUNTIME_DIRECTORY PROJECT_UUID [WIDTH HEIGHT FRAMES]
 * WIDTH/HEIGHT are output sizes; NGX supplies quality-mode input dimensions.
 * Defaults 640x360, 16 frames. Static analytic pre-HUD scene uses real zero
 * unjittered motion, Halton sample jitter, ordinary [0,1] perspective depth,
 * Linear color in RGBA16F with HDR-input and AutoExposure flags, R32F depth, RG32F motion,
 * and RGBA16F output. RGB is validated; output alpha is explicitly set to 1.
 * Writes PREFIX.input.ppm, PREFIX.dlss.ppm, PREFIX.reference.ppm and
 * PREFIX.dlss.rgba32f (little endian). No visual/performance/gameplay claims.
 * SDK/runtime distribution remains subject to the NVIDIA RTX SDK license.
 * No NVSDK_NGX_UpdateFeature call is made: documented optional OTA updates
 * are not requested. This does not assert control over driver-level behavior.
 */
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include "nvsdk_ngx_vk.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
struct Failure { int code; std::string message; };
static void vkcheck(VkResult r,const char *s) {
    if(r!=VK_SUCCESS) throw Failure{1,std::string(s)+": VkResult="+std::to_string(r)};
}
static void ngxcheck(NVSDK_NGX_Result r,const char *s,int code=1) {
    std::fprintf(stderr,"NGX %s = 0x%08x\n",s,static_cast<unsigned>(r));
    if(NVSDK_NGX_FAILED(r)) throw Failure{code,s};
}
static void require(bool condition,const char *s,int code=77) {if(!condition)throw Failure{code,s};}
static uint32_t number(const wchar_t *s,uint32_t low,uint32_t high) {
    wchar_t *end=nullptr;unsigned long n=std::wcstoul(s,&end,10);
    require(*s && !*end && *s!=L'-' && n>=low && n<=high,"invalid numeric argument",2);
    return static_cast<uint32_t>(n);
}
static std::string uuid(const wchar_t *s) {
    std::wstring text(s);require(text.size()==36,"PROJECT_UUID must be an application-specific UUID",2);
    std::string out;
    for(size_t i=0;i<text.size();i++) {
        bool dash=i==8 || i==13 || i==18 || i==23;
        require(dash?text[i]==L'-':((text[i]>=L'0'&&text[i]<=L'9')||(text[i]>=L'a'&&text[i]<=L'f')||(text[i]>=L'A'&&text[i]<=L'F')),"invalid PROJECT_UUID",2);
        out.push_back(static_cast<char>(text[i]));
    }
    return out;
}
static uint16_t half(float f) {
    // Fixture color is finite, nonnegative and in [0,1]. Round to nearest.
    uint32_t b=0;std::memcpy(&b,&f,4);
    int e=static_cast<int>((b>>23)&255)-127+15;
    uint32_t m=b&0x7fffff;
    if(e<=0) {if(e < -10)return 0;m=(m|0x800000)>>(1-e);return static_cast<uint16_t>((m+0x1000)>>13);}
    return static_cast<uint16_t>((static_cast<uint32_t>(e)<<10)+((m+0x1000)>>13));
}
static float unhalf(uint16_t h) {
    unsigned sign=h>>15,e=(h>>10)&31,m=h&1023;
    float value=e==0?std::ldexp(static_cast<float>(m),-24):e==31?(m?NAN:INFINITY):std::ldexp(1.0f+static_cast<float>(m)/1024,static_cast<int>(e)-15);
    return sign?-value:value;
}
struct Pixel {std::array<float,4> color;float depth;};
static Pixel fixture(float x,float y) {
    // Coordinates are normalized, top-left origin. Scene is completely static.
    float dx=x-.35f,dy=y-.48f;
    bool disc=dx*dx+dy*dy<.14f*.14f;
    bool diagonal=std::fabs(y-(.18f+.58f*x))<.008f;
    bool grid=(static_cast<int>(std::floor(x*24))+static_cast<int>(std::floor(y*16)))%2!=0;
    std::array<float,4> color={grid?.10f:.25f,.15f+.50f*x,.10f+.45f*y,1};
    if(diagonal)color={.95f,.80f,.10f,1};
    if(disc)color={.80f,.10f,.15f,1};
    float z=disc?2.0f:10.0f;
    return {color,100.0f/99.9f-10.0f/(99.9f*z)};
}
static float halton(uint32_t n,uint32_t base) {
    float result=0,f=1;
    while(n) {f/=static_cast<float>(base);result+=f*static_cast<float>(n%base);n/=base;}return result;
}
struct Buffer {VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;void *mapped=nullptr;};
struct Image {VkImage image=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;uint32_t width=0,height=0;VkFormat format=VK_FORMAT_UNDEFINED;};
struct App {
    VkInstance instance=VK_NULL_HANDLE;VkPhysicalDevice physical=VK_NULL_HANDLE;VkDevice device=VK_NULL_HANDLE;
    VkQueue queue=VK_NULL_HANDLE;uint32_t family=0;VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer command=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    NVSDK_NGX_Parameter *parameters=nullptr;NVSDK_NGX_Handle *feature=nullptr;bool initialized=false;
    std::wstring logText,runtimeText;const wchar_t *runtimePaths[1]{};
    NVSDK_NGX_FeatureCommonInfo common{};
    std::array<Image,4> images{};Buffer upload,readback;
    ~App() {
        if(device)vkDeviceWaitIdle(device);
        if(feature)NVSDK_NGX_VULKAN_ReleaseFeature(feature);
        if(parameters)NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        if(initialized)NVSDK_NGX_VULKAN_Shutdown1(device);
        if(device) {
            if(fence)vkDestroyFence(device,fence,nullptr);if(pool)vkDestroyCommandPool(device,pool,nullptr);
            for(auto &i:images){if(i.view)vkDestroyImageView(device,i.view,nullptr);if(i.image)vkDestroyImage(device,i.image,nullptr);if(i.memory)vkFreeMemory(device,i.memory,nullptr);}
            for(Buffer *b:{&upload,&readback}) {if(b->mapped)vkUnmapMemory(device,b->memory);if(b->buffer)vkDestroyBuffer(device,b->buffer,nullptr);if(b->memory)vkFreeMemory(device,b->memory,nullptr);}
            vkDestroyDevice(device,nullptr);
        }
        if(instance)vkDestroyInstance(instance,nullptr);
    }
    uint32_t memoryType(uint32_t bits,VkMemoryPropertyFlags flags) {
        for(uint32_t i=0;i<memory.memoryTypeCount;i++)if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags)return i;
        throw Failure{77,"required Vulkan memory type unavailable"};
    }
    void makeBuffer(Buffer &b,VkDeviceSize bytes,VkBufferUsageFlags usage) {
        VkBufferCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;c.size=bytes;c.usage=usage;c.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        vkcheck(vkCreateBuffer(device,&c,nullptr,&b.buffer),"create buffer");
        VkMemoryRequirements r{};vkGetBufferMemoryRequirements(device,b.buffer,&r);
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkcheck(vkAllocateMemory(device,&a,nullptr,&b.memory),"allocate buffer");vkcheck(vkBindBufferMemory(device,b.buffer,b.memory,0),"bind buffer");vkcheck(vkMapMemory(device,b.memory,0,VK_WHOLE_SIZE,0,&b.mapped),"map buffer");
    }
    void makeImage(Image &i,uint32_t w,uint32_t h,VkFormat format) {
        i.width=w;i.height=h;i.format=format;
        VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(physical,format,&p);
        VkFormatFeatureFlags wanted=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        require((p.optimalTilingFeatures&wanted)==wanted,"required DLSS image format capabilities unavailable");
        VkImageCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;c.imageType=VK_IMAGE_TYPE_2D;c.format=format;c.extent={w,h,1};c.mipLevels=1;c.arrayLayers=1;c.samples=VK_SAMPLE_COUNT_1_BIT;c.tiling=VK_IMAGE_TILING_OPTIMAL;c.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;c.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        vkcheck(vkCreateImage(device,&c,nullptr,&i.image),"create image");VkMemoryRequirements r{};vkGetImageMemoryRequirements(device,i.image,&r);
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkcheck(vkAllocateMemory(device,&a,nullptr,&i.memory),"allocate image");vkcheck(vkBindImageMemory(device,i.image,i.memory,0),"bind image");
        VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=i.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=format;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkcheck(vkCreateImageView(device,&v,nullptr,&i.view),"create image view");
    }
    void begin() {
        vkcheck(vkResetCommandPool(device,pool,0),"reset command pool");
        VkCommandBufferBeginInfo b{};b.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkcheck(vkBeginCommandBuffer(command,&b),"begin commands");
    }
    void finish() {
        vkcheck(vkEndCommandBuffer(command),"end commands");vkcheck(vkResetFences(device,1,&fence),"reset fence");
        VkSubmitInfo s{};s.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;s.commandBufferCount=1;s.pCommandBuffers=&command;
        vkcheck(vkQueueSubmit(queue,1,&s,fence),"submit commands");vkcheck(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"wait commands");
    }
    void transition(Image &i,VkImageLayout oldLayout,VkImageLayout newLayout,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags srcStage,VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcAccessMask=src;b.dstAccessMask=dst;b.oldLayout=oldLayout;b.newLayout=newLayout;b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=i.image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(command,srcStage,dstStage,0,0,nullptr,0,nullptr,1,&b);
    }
    NVSDK_NGX_Resource_VK resource(Image &i,bool write) {
        NVSDK_NGX_Resource_VK r{};r.Type=NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;r.ReadWrite=write;
        r.Resource.ImageViewInfo={i.view,i.image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},i.format,i.width,i.height};return r;
    }
};
static std::vector<const char*> extensionNames(uint32_t count,VkExtensionProperties *extensions) {
    require(count<=256 && (!count||extensions),"malformed NGX extension requirements");
    std::vector<const char*> names;for(uint32_t i=0;i<count;i++)names.push_back(extensions[i].extensionName);return names;
}
static void verifyExtensions(const std::vector<const char*> &wanted,const std::vector<VkExtensionProperties> &available) {
    for(const char *name:wanted) {
        bool found=false;for(const auto &p:available)if(!std::strcmp(name,p.extensionName))found=true;
        if(!found)throw Failure{77,std::string("required extension unavailable: ")+name};
    }
}
static void initialize(App &a,const fs::path &logs,const fs::path &runtime,const std::string &project) {
    a.logText=logs.wstring();a.runtimeText=runtime.wstring();a.runtimePaths[0]=a.runtimeText.c_str();
    a.common.PathListInfo.Path=a.runtimePaths;a.common.PathListInfo.Length=1;
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};discovery.SDKVersion=NVSDK_NGX_Version_API;discovery.FeatureID=NVSDK_NGX_Feature_SuperSampling;
    discovery.Identifier.IdentifierType=NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc={project.c_str(),NVSDK_NGX_ENGINE_TYPE_CUSTOM,"offline-static-diagnostic-1"};
    discovery.ApplicationDataPath=a.logText.c_str();discovery.FeatureInfo=&a.common;
    uint32_t count=0;VkExtensionProperties *required=nullptr;
    ngxcheck(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery,&count,&required),"instance extension requirements",77);
    auto names=extensionNames(count,required);
    uint32_t availableCount=0;vkcheck(vkEnumerateInstanceExtensionProperties(nullptr,&availableCount,nullptr),"instance extensions");
    std::vector<VkExtensionProperties> available(availableCount);vkcheck(vkEnumerateInstanceExtensionProperties(nullptr,&availableCount,available.data()),"instance extensions");verifyExtensions(names,available);
    VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.pApplicationName="Offline DLSS SR static fixture";app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ic{};ic.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ic.pApplicationInfo=&app;ic.enabledExtensionCount=static_cast<uint32_t>(names.size());ic.ppEnabledExtensionNames=names.data();
    vkcheck(vkCreateInstance(&ic,nullptr,&a.instance),"create Vulkan instance");
    vkcheck(vkEnumeratePhysicalDevices(a.instance,&count,nullptr),"physical devices");require(count>0 && count<=256,"no bounded physical device inventory");
    std::vector<VkPhysicalDevice> devices(count);vkcheck(vkEnumeratePhysicalDevices(a.instance,&count,devices.data()),"physical devices");
    for(auto p:devices) {
        VkPhysicalDeviceProperties prop{};vkGetPhysicalDeviceProperties(p,&prop);if(prop.vendorID!=0x10de || prop.apiVersion<VK_API_VERSION_1_2)continue;
        NVSDK_NGX_FeatureRequirement support{};auto result=NVSDK_NGX_VULKAN_GetFeatureRequirements(a.instance,p,&discovery,&support);
        std::fprintf(stderr,"NGX support %s: result=0x%08x flags=0x%x\n",prop.deviceName,static_cast<unsigned>(result),static_cast<unsigned>(support.FeatureSupported));
        if(NVSDK_NGX_FAILED(result) || support.FeatureSupported!=NVSDK_NGX_FeatureSupportResult_Supported)continue;
        uint32_t queues=0;vkGetPhysicalDeviceQueueFamilyProperties(p,&queues,nullptr);std::vector<VkQueueFamilyProperties> q(queues);vkGetPhysicalDeviceQueueFamilyProperties(p,&queues,q.data());
        for(uint32_t j=0;j<queues;j++)if(q[j].queueCount && (q[j].queueFlags&(VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT))==(VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT)){a.physical=p;a.family=j;break;}
        if(a.physical) {std::fprintf(stderr,"Selected %s\n",prop.deviceName);break;}
    }
    require(a.physical!=VK_NULL_HANDLE,"NGX DLSS SR support unavailable");
    ngxcheck(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(a.instance,a.physical,&discovery,&count,&required),"device extension requirements",77);names=extensionNames(count,required);
    vkcheck(vkEnumerateDeviceExtensionProperties(a.physical,nullptr,&availableCount,nullptr),"device extensions");available.resize(availableCount);vkcheck(vkEnumerateDeviceExtensionProperties(a.physical,nullptr,&availableCount,available.data()),"device extensions");verifyExtensions(names,available);
    VkPhysicalDeviceFeatures2 f{};f.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    VkPhysicalDeviceVulkan11Features f11{};f11.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};f12.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;f.pNext=&f11;f11.pNext=&f12;vkGetPhysicalDeviceFeatures2(a.physical,&f);
    float priority=1;VkDeviceQueueCreateInfo qc{};qc.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;qc.queueFamilyIndex=a.family;qc.queueCount=1;qc.pQueuePriorities=&priority;
    VkDeviceCreateInfo dc{};dc.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;dc.pNext=&f;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;dc.enabledExtensionCount=static_cast<uint32_t>(names.size());dc.ppEnabledExtensionNames=names.data();
    vkcheck(vkCreateDevice(a.physical,&dc,nullptr,&a.device),"create Vulkan device");vkGetDeviceQueue(a.device,a.family,0,&a.queue);vkGetPhysicalDeviceMemoryProperties(a.physical,&a.memory);
    ngxcheck(NVSDK_NGX_VULKAN_Init_with_ProjectID(project.c_str(),NVSDK_NGX_ENGINE_TYPE_CUSTOM,"offline-static-diagnostic-1",a.logText.c_str(),a.instance,a.physical,a.device,vkGetInstanceProcAddr,vkGetDeviceProcAddr,&a.common),"initialize",77);a.initialized=true;
    ngxcheck(NVSDK_NGX_VULKAN_GetCapabilityParameters(&a.parameters),"capability parameters",77);
    int supported=0,needsDriver=0;
    ngxcheck(a.parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available,&supported),"SuperSampling.Available",77);
    a.parameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,&needsDriver);
    std::fprintf(stderr,"DLSS SR available=%d needsUpdatedDriver=%d\n",supported,needsDriver);require(supported && !needsDriver,"DLSS SR unavailable or driver update required");
    VkCommandPoolCreateInfo pc{};pc.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pc.queueFamilyIndex=a.family;vkcheck(vkCreateCommandPool(a.device,&pc,nullptr,&a.pool),"create command pool");
    VkCommandBufferAllocateInfo bc{};bc.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;bc.commandPool=a.pool;bc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;bc.commandBufferCount=1;vkcheck(vkAllocateCommandBuffers(a.device,&bc,&a.command),"allocate commands");
    VkFenceCreateInfo fc{};fc.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;vkcheck(vkCreateFence(a.device,&fc,nullptr,&a.fence),"create fence");
}
static void writePPM(const fs::path &path,const std::vector<float> &rgba,uint32_t w,uint32_t h) {
    FILE *file=nullptr;require(_wfopen_s(&file,path.c_str(),L"wb")==0,"open PPM failed",1);
    bool ok=std::fprintf(file,"P6\n%u %u\n255\n",w,h)>0;
    for(size_t i=0;i<static_cast<size_t>(w)*h && ok;i++) {
        unsigned char rgb[3];for(size_t j=0;j<3;j++) {float v=std::clamp(rgba[i*4+j],0.0f,1.0f);v=v<=.0031308f?12.92f*v:1.055f*std::pow(v,1.0f/2.4f)-.055f;rgb[j]=static_cast<unsigned char>(std::lround(v*255));}
        ok=std::fwrite(rgb,1,3,file)==3;
    }
    if(std::fclose(file))ok=false;require(ok,"PPM write failed",1);
}
static void runStatic(App &a,const fs::path &prefix,uint32_t width,uint32_t height,uint32_t frames) {
    auto p=a.parameters;void *callback=nullptr;
    ngxcheck(p->Get(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback,&callback),"optimal settings callback",77);require(callback!=nullptr,"optimal settings callback unavailable");
    p->Set(NVSDK_NGX_Parameter_Width,width);p->Set(NVSDK_NGX_Parameter_Height,height);p->Set(NVSDK_NGX_Parameter_PerfQualityValue,static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxQuality));p->Set(NVSDK_NGX_Parameter_RTXValue,0);
    ngxcheck(reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(callback)(p),"optimal settings",77);
    uint32_t iw=0,ih=0;ngxcheck(p->Get(NVSDK_NGX_Parameter_OutWidth,&iw),"input width");ngxcheck(p->Get(NVSDK_NGX_Parameter_OutHeight,&ih),"input height");
    require(iw>0 && ih>0 && iw<=width && ih<=height,"invalid quality-mode dimensions");
    std::fprintf(stderr,"Quality mode %ux%u -> %ux%u, %u static frames\n",iw,ih,width,height,frames);
    a.makeImage(a.images[0],iw,ih,VK_FORMAT_R16G16B16A16_SFLOAT);a.makeImage(a.images[1],iw,ih,VK_FORMAT_R32_SFLOAT);a.makeImage(a.images[2],iw,ih,VK_FORMAT_R32G32_SFLOAT);a.makeImage(a.images[3],width,height,VK_FORMAT_R16G16B16A16_SFLOAT);
    size_t inputCount=static_cast<size_t>(iw)*ih,outputCount=static_cast<size_t>(width)*height;
    VkDeviceSize colorBytes=inputCount*8,depthBytes=inputCount*4,motionBytes=inputCount*8;
    // RG32F copies require an 8-byte texel-block-aligned buffer offset, including
    // odd input pixel counts where the preceding R32F region ends at offset 4 mod 8.
    VkDeviceSize motionOffset=(colorBytes+depthBytes+7)&~VkDeviceSize(7);
    a.makeBuffer(a.upload,motionOffset+motionBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);a.makeBuffer(a.readback,outputCount*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    p->Set(NVSDK_NGX_Parameter_Width,iw);p->Set(NVSDK_NGX_Parameter_Height,ih);p->Set(NVSDK_NGX_Parameter_OutWidth,width);p->Set(NVSDK_NGX_Parameter_OutHeight,height);p->Set(NVSDK_NGX_Parameter_CreationNodeMask,1u);p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask,1u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,static_cast<int>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_IsHDR|NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,0);
    size_t scratch=0;ngxcheck(NVSDK_NGX_VULKAN_GetScratchBufferSize(NVSDK_NGX_Feature_SuperSampling,p,&scratch),"scratch requirements");require(scratch==0,"this diagnostic requires the documented no-scratch DLSS SR path");
    a.begin();ngxcheck(NVSDK_NGX_VULKAN_CreateFeature1(a.device,a.command,NVSDK_NGX_Feature_SuperSampling,p,&a.feature),"create DLSS SR");a.finish();
    std::array<NVSDK_NGX_Resource_VK,4> resources;for(size_t i=0;i<4;i++)resources[i]=a.resource(a.images[i],i==3);
    p->Set(NVSDK_NGX_Parameter_Color,static_cast<void*>(&resources[0]));p->Set(NVSDK_NGX_Parameter_Depth,static_cast<void*>(&resources[1]));p->Set(NVSDK_NGX_Parameter_MotionVectors,static_cast<void*>(&resources[2]));p->Set(NVSDK_NGX_Parameter_Output,static_cast<void*>(&resources[3]));
    p->Set(NVSDK_NGX_Parameter_MV_Scale_X,1.0f);p->Set(NVSDK_NGX_Parameter_MV_Scale_Y,1.0f);p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure,1.0f);p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale,1.0f);p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,1000.0f/60);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,iw);p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,ih);p->Set(NVSDK_NGX_Parameter_Sharpness,0.0f);
    std::vector<float> input(inputCount*4),output(outputCount*4),reference(outputCount*4);
    for(uint32_t frame=0;frame<frames;frame++) {
        float jx=halton(frame+1,2)-.5f,jy=halton(frame+1,3)-.5f;
        auto bytes=static_cast<unsigned char*>(a.upload.mapped);auto color=reinterpret_cast<uint16_t*>(bytes);auto depth=reinterpret_cast<float*>(bytes+colorBytes);
        std::memset(bytes+motionOffset,0,static_cast<size_t>(motionBytes)); // Static world/camera; jitter excluded from MVs.
        for(uint32_t y=0;y<ih;y++)for(uint32_t x=0;x<iw;x++) {
            // Positive viewport jitter shifts the image right/down: evaluate the
            // unjittered scene at pixel-jitter, and report that viewport offset.
            Pixel v=fixture((static_cast<float>(x)+.5f-jx)/static_cast<float>(iw),(static_cast<float>(y)+.5f-jy)/static_cast<float>(ih));size_t i=static_cast<size_t>(y)*iw+x;depth[i]=v.depth;
            for(size_t c=0;c<4;c++){color[i*4+c]=half(v.color[c]);input[i*4+c]=v.color[c];}
        }
        p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X,jx);p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,jy);p->Set(NVSDK_NGX_Parameter_Reset,frame==0?1:0);
        a.begin();
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;vkCmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&host,0,nullptr,0,nullptr);
        for(size_t i=0;i<3;i++) {
            a.transition(a.images[i],frame?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,frame?VK_ACCESS_SHADER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT,frame?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};copy.bufferOffset=i==0?0:i==1?colorBytes:motionOffset;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={iw,ih,1};
            vkCmdCopyBufferToImage(a.command,a.upload.buffer,a.images[i].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            a.transition(a.images[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        a.transition(a.images[3],frame?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,frame?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_SHADER_WRITE_BIT,frame?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ngxcheck(NVSDK_NGX_VULKAN_EvaluateFeature_C(a.command,a.feature,p,nullptr),"evaluate DLSS SR");
        a.transition(a.images[3],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};vkCmdCopyImageToBuffer(a.command,a.images[3].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,a.readback.buffer,1,&copy);
        VkMemoryBarrier read{};read.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;read.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;read.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&read,0,nullptr,0,nullptr);
        a.finish();
        auto result=static_cast<const uint16_t*>(a.readback.mapped);for(size_t i=0;i<output.size();i++) {output[i]=(i%4==3)?1.0f:unhalf(result[i]);require(std::isfinite(output[i]),"nonfinite DLSS RGB output",1);}
    }
    double mse=0;float minimum=INFINITY,maximum=-INFINITY;
    for(uint32_t y=0;y<height;y++)for(uint32_t x=0;x<width;x++) {
        // 4x4 spatial reference is diagnostic, not a perceptual quality verdict.
        size_t i=(static_cast<size_t>(y)*width+x)*4;
        for(uint32_t sy=0;sy<4;sy++)for(uint32_t sx=0;sx<4;sx++) {
            Pixel v=fixture((static_cast<float>(x)+(static_cast<float>(sx)+.5f)/4)/static_cast<float>(width),(static_cast<float>(y)+(static_cast<float>(sy)+.5f)/4)/static_cast<float>(height));
            for(size_t c=0;c<4;c++)reference[i+c]+=v.color[c]/16;
        }
        for(size_t c=0;c<3;c++){double d=output[i+c]-reference[i+c];mse+=d*d;minimum=std::min(minimum,output[i+c]);maximum=std::max(maximum,output[i+c]);}
    }
    require(maximum-minimum>.05f,"DLSS output is unexpectedly constant",1);
    writePPM(prefix.wstring()+L".input.ppm",input,iw,ih);writePPM(prefix.wstring()+L".dlss.ppm",output,width,height);writePPM(prefix.wstring()+L".reference.ppm",reference,width,height);
    FILE *raw=nullptr;require(_wfopen_s(&raw,(prefix.wstring()+L".dlss.rgba32f").c_str(),L"wb")==0,"open raw output failed",1);bool ok=std::fwrite(output.data(),4,output.size(),raw)==output.size();if(std::fclose(raw))ok=false;require(ok,"write raw output failed",1);
    std::fprintf(stdout,"DLSS SR created/evaluated/read back: %u static frames, RGB range [%g,%g], reference RGB MSE=%g.\n",frames,static_cast<double>(minimum),static_cast<double>(maximum),mse/static_cast<double>(outputCount*3));
    std::fprintf(stdout,"Synthetic pre-HUD fixture only. No gameplay, temporal motion quality, performance, FG or RR validation.\n");
    HMODULE module=GetModuleHandleW(L"nvngx_dlss.dll");wchar_t loaded[32768]{};if(module && GetModuleFileNameW(module,loaded,32768))std::fprintf(stderr,"Loaded SR runtime: %s\n",fs::path(loaded).u8string().c_str());else std::fprintf(stderr,"Loaded SR module path unavailable; inspect NGX logs before runtime attribution.\n");
}
int wmain(int argc,wchar_t **argv) {
    try {
        bool probe=argc==5 && !std::wcscmp(argv[1],L"--probe");
        bool render=(argc==6 || argc==9) && !std::wcscmp(argv[1],L"--static");
        if(!probe && !render) {std::fprintf(stderr,"Usage: test_vulkan_dlss_sr --probe LOG_DIR RUNTIME_DIR PROJECT_UUID\n       test_vulkan_dlss_sr --static PREFIX LOG_DIR RUNTIME_DIR PROJECT_UUID [WIDTH HEIGHT FRAMES]\nPrivate static-fixture diagnostic, no game inputs. Exit77=unavailable,2=usage,1=failed.\n");return 2;}
        int offset=probe?0:1;fs::path logs=fs::absolute(argv[2+offset]),runtime=fs::absolute(argv[3+offset]);std::string project=uuid(argv[4+offset]);
        require(fs::is_directory(runtime) && fs::is_regular_file(runtime/L"nvngx_dlss.dll"),"explicit runtime directory must contain official nvngx_dlss.dll",2);fs::create_directories(logs);
        uint32_t width=640,height=360,frames=16;if(argc==9){width=number(argv[6],64,2048);height=number(argv[7],64,2048);frames=number(argv[8],1,64);}
        App a;initialize(a,logs,runtime,project);
        if(probe) {std::puts("NGX DLSS SR support and capability queries passed; no feature evaluation performed.");return 0;}
        fs::path prefix=fs::absolute(argv[2]);require(fs::is_directory(prefix.parent_path()),"output parent directory does not exist",2);runStatic(a,prefix,width,height,frames);return 0;
    } catch(const Failure &f){std::fprintf(stderr,"%s: %s\n",f.code==77?"UNAVAILABLE":"FAILED",f.message.c_str());return f.code;}
      catch(const std::exception &e){std::fprintf(stderr,"FAILED: %s\n",e.what());return 1;}
}

// Controller-authored after managed fixture job-IywAvw timed out with no artifact.
// Actual public CACAO Vulkan library, synthetic inputs, no Folklore data.
#include <vulkan/vulkan.h>
#include "ffx_cacao_impl.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

static unsigned checks;
static std::atomic<unsigned> validationErrors(0);
static void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
#define VK(call) check((call) == VK_SUCCESS, #call)
static VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validationErrors; std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    uint32_t family = 0;
    ~Device() {
        if (device) { vkDeviceWaitIdle(device); if (pool) vkDestroyCommandPool(device,pool,nullptr); vkDestroyDevice(device,nullptr); }
        if (messenger) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) destroy(instance,messenger,nullptr);
        }
        if (instance) vkDestroyInstance(instance,nullptr);
    }
    void init() {
        uint32_t count=0; VK(vkEnumerateInstanceLayerProperties(&count,nullptr));
        std::vector<VkLayerProperties> layers(count); VK(vkEnumerateInstanceLayerProperties(&count,layers.data()));
        bool validation=false;
        for (const auto& layer:layers) if (!std::strcmp(layer.layerName,"VK_LAYER_KHRONOS_validation")) validation=true;
        VK(vkEnumerateInstanceExtensionProperties(nullptr,&count,nullptr));
        std::vector<VkExtensionProperties> extensions(count); VK(vkEnumerateInstanceExtensionProperties(nullptr,&count,extensions.data()));
        bool debug=false; for(const auto& extension:extensions) if(!std::strcmp(extension.extensionName,VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) debug=true;
        const char* layer="VK_LAYER_KHRONOS_validation"; const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkApplicationInfo app={}; app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; app.pApplicationName="CACAO component test"; app.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo info={}; info.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; info.pApplicationInfo=&app;
        if(validation && debug) { info.enabledLayerCount=1; info.ppEnabledLayerNames=&layer; info.enabledExtensionCount=1; info.ppEnabledExtensionNames=&extension; }
        VK(vkCreateInstance(&info,nullptr,&instance));
        std::printf("Validation layer: %s\n",validation && debug ? "enabled" : "unavailable; not tested");
        if(validation && debug) {
            auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT")); check(create!=nullptr,"debug utils entry point");
            VkDebugUtilsMessengerCreateInfoEXT di={}; di.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            di.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT; di.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT; di.pfnUserCallback=debugMessage;
            VK(create(instance,&di,nullptr,&messenger));
        }
        VK(vkEnumeratePhysicalDevices(instance,&count,nullptr)); check(count>0,"no physical device from selected ICD");
        std::vector<VkPhysicalDevice> devices(count); VK(vkEnumeratePhysicalDevices(instance,&count,devices.data()));
        for(auto candidate:devices) {
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&count,nullptr); std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&count,families.data());
            for(uint32_t i=0;i<count;++i) if(families[i].queueCount && (families[i].queueFlags&VK_QUEUE_COMPUTE_BIT)) { physical=candidate; family=i; break; }
            if(physical) break;
        }
        check(physical!=VK_NULL_HANDLE,"no compute queue");
        VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical,&properties); std::printf("Device: %s\n",properties.deviceName);
        VkPhysicalDeviceFeatures supported; vkGetPhysicalDeviceFeatures(physical,&supported); check(supported.shaderStorageImageExtendedFormats,"storage image extended formats required");
        VkPhysicalDeviceFeatures enabled={}; enabled.shaderStorageImageExtendedFormats=VK_TRUE;
        float priority=1; VkDeviceQueueCreateInfo qi={}; qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
        VkDeviceCreateInfo dc={}; dc.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&qi; dc.pEnabledFeatures=&enabled;
        VK(vkCreateDevice(physical,&dc,nullptr,&device)); vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo pc={}; pc.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pc.queueFamilyIndex=family; pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK(vkCreateCommandPool(device,&pc,nullptr,&pool)); VkCommandBufferAllocateInfo ca={}; ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ca.commandPool=pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
        VK(vkAllocateCommandBuffers(device,&ca,&command));
        for(VkFormat format:{VK_FORMAT_R16_SFLOAT,VK_FORMAT_R8G8B8A8_SNORM,VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8_UNORM,VK_FORMAT_R32_UINT}) {
            VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(physical,format,&fp);
            check((fp.optimalTilingFeatures&(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))==(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT),"CACAO internal sampled/storage format unsupported");
        }
    }
    uint32_t memory(uint32_t bits,VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties p; vkGetPhysicalDeviceMemoryProperties(physical,&p);
        for(uint32_t i=0;i<p.memoryTypeCount;++i) if((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags) return i;
        throw std::runtime_error("required memory type unavailable");
    }
    void begin() { VK(vkResetCommandBuffer(command,0)); VkCommandBufferBeginInfo b={}; b.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; VK(vkBeginCommandBuffer(command,&b)); }
    void submit() { VK(vkEndCommandBuffer(command)); VkSubmitInfo s={}; s.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; s.commandBufferCount=1; s.pCommandBuffers=&command; VK(vkQueueSubmit(queue,1,&s,VK_NULL_HANDLE)); VK(vkQueueWaitIdle(queue)); }
};

struct Buffer {
    Device& d; VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; void* mapped=nullptr;
    explicit Buffer(Device& device):d(device) {}
    ~Buffer() { if(mapped)vkUnmapMemory(d.device,memory); if(buffer)vkDestroyBuffer(d.device,buffer,nullptr); if(memory)vkFreeMemory(d.device,memory,nullptr); }
    void init(size_t bytes,VkBufferUsageFlags usage) {
        VkBufferCreateInfo b={}; b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; b.size=bytes; b.usage=usage; b.sharingMode=VK_SHARING_MODE_EXCLUSIVE; VK(vkCreateBuffer(d.device,&b,nullptr,&buffer));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(d.device,buffer,&req); VkMemoryAllocateInfo a={}; a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; a.allocationSize=req.size; a.memoryTypeIndex=d.memory(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK(vkAllocateMemory(d.device,&a,nullptr,&memory)); VK(vkBindBufferMemory(d.device,buffer,memory,0)); VK(vkMapMemory(d.device,memory,0,bytes,0,&mapped));
    }
};
struct Image {
    Device& d; VkImage image=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
    explicit Image(Device& device):d(device) {}
    ~Image() { if(view)vkDestroyImageView(d.device,view,nullptr); if(image)vkDestroyImage(d.device,image,nullptr); if(memory)vkFreeMemory(d.device,memory,nullptr); }
    void init(uint32_t w,uint32_t h,VkFormat format,VkImageUsageFlags usage) {
        VkImageCreateInfo i={}; i.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; i.imageType=VK_IMAGE_TYPE_2D; i.format=format; i.extent={w,h,1}; i.mipLevels=1; i.arrayLayers=1; i.samples=VK_SAMPLE_COUNT_1_BIT; i.tiling=VK_IMAGE_TILING_OPTIMAL; i.usage=usage; i.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VK(vkCreateImage(d.device,&i,nullptr,&image)); VkMemoryRequirements req; vkGetImageMemoryRequirements(d.device,image,&req); VkMemoryAllocateInfo a={}; a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; a.allocationSize=req.size; a.memoryTypeIndex=d.memory(req.memoryTypeBits,0);
        VK(vkAllocateMemory(d.device,&a,nullptr,&memory)); VK(vkBindImageMemory(d.device,image,memory,0));
        VkImageViewCreateInfo v={}; v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; v.image=image; v.viewType=VK_IMAGE_VIEW_TYPE_2D; v.format=format; v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; VK(vkCreateImageView(d.device,&v,nullptr,&view));
    }
    void transition(VkImageLayout next) {
        VkImageMemoryBarrier b={}; b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b.oldLayout=layout; b.newLayout=next; b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=image; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.srcAccessMask=layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT; b.dstAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(d.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b); layout=next;
    }
};
struct Cacao {
    Device& d; FFX_CACAO_VkContext* context=nullptr; bool ready=false;
    explicit Cacao(Device& device):d(device) {}
    ~Cacao() { if(ready)FFX_CACAO_VkDestroyContext(context); std::free(context); }
    void init() { context=static_cast<FFX_CACAO_VkContext*>(std::calloc(1,FFX_CACAO_VkGetContextSize())); check(context!=nullptr,"CACAO context allocation"); FFX_CACAO_VkCreateInfo i={}; i.device=d.device; i.physicalDevice=d.physical; check(FFX_CACAO_VkInitContext(context,&i)==FFX_CACAO_STATUS_OK,"CACAO context init"); ready=true; }
};
struct Screen {
    Cacao& c; bool ready=false;
    explicit Screen(Cacao& cacao):c(cacao) {}
    ~Screen() { if(ready) { vkDeviceWaitIdle(c.d.device); FFX_CACAO_VkDestroyScreenSizeDependentResources(c.context); } }
};
static void save(const std::string& dir,const std::string& name,uint32_t w,uint32_t h,const std::vector<unsigned char>& pixels) {
    if(dir.empty()) return;
    FILE* f=std::fopen((dir+"/"+name).c_str(),"wb"); check(f!=nullptr,"PGM open");
    bool ok=std::fprintf(f,"P5\n%u %u\n255\n",w,h)>0 && std::fwrite(pixels.data(),1,pixels.size(),f)==pixels.size();
    if(std::fclose(f))ok=false; check(ok,"PGM write");
}
static void runSize(Device& d,Cacao& c,uint32_t w,uint32_t h,const std::string& dir) {
    const size_t count=size_t(w)*h;
    Image depth(d),output(d); Buffer upload(d),readback(d);
    depth.init(w,h,VK_FORMAT_R32_SFLOAT,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    output.init(w,h,VK_FORMAT_R8_UNORM,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    upload.init(count*sizeof(float),VK_BUFFER_USAGE_TRANSFER_SRC_BIT); readback.init(count,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Screen screen(c); FFX_CACAO_VkScreenSizeInfo si={}; si.width=w; si.height=h; si.depthView=depth.view; si.output=output.image; si.outputView=output.view; si.useDownsampledSsao=FFX_CACAO_FALSE;
    check(FFX_CACAO_VkInitScreenSizeDependentResources(c.context,&si)==FFX_CACAO_STATUS_OK,"CACAO screen init"); screen.ready=true;
    FFX_CACAO_Settings settings=FFX_CACAO_DEFAULT_SETTINGS; settings.generateNormals=FFX_CACAO_TRUE; settings.qualityLevel=FFX_CACAO_QUALITY_HIGH; settings.blurPassCount=2; settings.detailShadowStrength=0.5f;
    check(FFX_CACAO_VkUpdateSettings(c.context,&settings)==FFX_CACAO_STATUS_OK,"CACAO settings");
    FFX_CACAO_Matrix4x4 projection={},normal={}; const float near=0.1f,far=100.f,tanHalf=std::tan(3.14159265358979323846f/6.f);
    projection.elements[0][0]=1.f/(tanHalf*float(w)/float(h)); projection.elements[1][1]=1.f/tanHalf; projection.elements[2][2]=far/(far-near); projection.elements[2][3]=1; projection.elements[3][2]=-far*near/(far-near);
    for(int i=0;i<4;++i)normal.elements[i][i]=1;
    auto render=[&](bool step) {
        auto values=static_cast<float*>(upload.mapped);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) { float z=step && x>=w/4 && x<3*w/4 && y>=h/4 && y<3*h/4?3.f:4.f; values[size_t(y)*w+x]=projection.elements[2][2]+projection.elements[3][2]/z; }
        std::memset(readback.mapped,0xcd,count); d.begin(); depth.transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region={}; region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.imageExtent={w,h,1}; vkCmdCopyBufferToImage(d.command,upload.buffer,depth.image,depth.layout,1,&region); depth.transition(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        check(FFX_CACAO_VkDraw(c.context,d.command,&projection,&normal)==FFX_CACAO_STATUS_OK,"CACAO actual draw");
        output.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; output.transition(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vkCmdCopyImageToBuffer(d.command,output.image,output.layout,readback.buffer,1,&region);
        VkBufferMemoryBarrier b={}; b.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER; b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask=VK_ACCESS_HOST_READ_BIT; b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.buffer=readback.buffer; b.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(d.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&b,0,nullptr); output.transition(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); d.submit();
        auto bytes=static_cast<unsigned char*>(readback.mapped); return std::vector<unsigned char>(bytes,bytes+count);
    };
    auto flat=render(false),step=render(true),repeat=render(true);
    double interior=0;size_t n=0,darker=0;
    for(uint32_t y=8;y<h-8;++y)for(uint32_t x=8;x<w-8;++x) { size_t p=size_t(y)*w+x;interior+=flat[p];++n;if(int(flat[p])-int(step[p])>=8)++darker; }
    auto range=std::minmax_element(step.begin(),step.end()); double sum=0;for(auto x:step)sum+=x;
    std::printf("%ux%u: flat interior %.3f; step min %u max %u mean %.3f; darker pixels %zu\n",w,h,interior/n,*range.first,*range.second,sum/count,darker);
    save(dir,"flat"+std::to_string(w)+".pgm",w,h,flat);save(dir,"step"+std::to_string(w)+".pgm",w,h,step);
    check(interior/n>=250,"flat plane unexpectedly occluded");check(darker>=4,"step geometry did not produce meaningful AO");check(*range.first<*range.second,"constant AO output");check(step==repeat,"fixed-input output is not deterministic");check(validationErrors.load()==0,"Vulkan validation errors");
}
int main(int argc,char** argv) {
    try {
        check(argc<=2,"usage: test_cacao_vulkan [existing-output-directory]");
        {
            Device device;device.init(); Cacao cacao(device);cacao.init();
            const std::string dir=argc==2?argv[1]:"";
            runSize(device,cacao,64,64,dir);runSize(device,cacao,96,64,dir);
        }
        check(validationErrors.load()==0,"Vulkan validation errors");
        std::printf("CACAO actual Vulkan: %u checks passed\n",checks);return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}

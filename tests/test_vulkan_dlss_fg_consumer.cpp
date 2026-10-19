/* Private Vulkan DLSS Frame Generation GPU fixture. It records and reads back
 * interpolated images; it never presents them or asserts gameplay quality.
 * Usage: test_vulkan_dlss_fg_consumer OUTPUT_PREFIX LOG_DIR RUNTIME_DIR
 *        PROJECT_UUID WIDTH HEIGHT FRAMES  (FRAMES >= 4)
 * Build through the bridge with pinned NGX headers/import library and Vulkan.
 */
#define wmain unused_base_sr_wmain
#include "test_vulkan_dlss_sr.cpp"
#undef wmain
#include "../libs/video/rsx_dlss_vk.h"

static rsx_dlss_identity fgIdentity(const App &a,const std::string &project) {
    return {project.c_str(),reinterpret_cast<const uint16_t*>(a.logText.c_str()),
            reinterpret_cast<const uint16_t*>(a.runtimeText.c_str())};
}
static void fgStatus(rsx_dlss_status status,const rsx_dlss_fg_report &r,
                     const char *step) {
    if(status==RSX_DLSS_OK)return;
    std::fprintf(stderr,"FG %s: status=%d cause=%d NGX=0x%08x flags=0x%x available=%u driver=%u\n",
                 step,static_cast<int>(status),static_cast<int>(r.cause),
                 static_cast<unsigned>(r.ngx_result),r.support_flags,r.available,
                 r.needs_driver_update);
    bool unavailable=status==RSX_DLSS_UNAVAILABLE &&
        (r.cause==RSX_DLSS_FG_RUNTIME_MISSING ||
         r.cause==RSX_DLSS_FG_UNSUPPORTED_DEVICE ||
         r.cause==RSX_DLSS_FG_DRIVER_UPDATE ||
         r.cause==RSX_DLSS_FG_FEATURE_UNAVAILABLE);
    throw Failure{unavailable?77:1,std::string(step)+": FG consumer unavailable or failed"};
}
struct FGConsumer {
    rsx_dlss_vk *value=nullptr;
    VkDevice device=VK_NULL_HANDLE;
    ~FGConsumer(){if(value){if(device)vkDeviceWaitIdle(device);rsx_dlss_destroy(value);}}
};
struct HUDImage {
    Image value;
    VkDevice device=VK_NULL_HANDLE;
    ~HUDImage(){if(device){vkDeviceWaitIdle(device);
        if(value.view)vkDestroyImageView(device,value.view,nullptr);
        if(value.image)vkDestroyImage(device,value.image,nullptr);
        if(value.memory)vkFreeMemory(device,value.memory,nullptr);}}
};
struct FGFlagBuffer {
    Buffer value;
    VkDevice device=VK_NULL_HANDLE;
    ~FGFlagBuffer(){if(device){vkDeviceWaitIdle(device);
        if(value.mapped)vkUnmapMemory(device,value.memory);
        if(value.buffer)vkDestroyBuffer(device,value.buffer,nullptr);
        if(value.memory)vkFreeMemory(device,value.memory,nullptr);}}
};
static void addExtensions(std::vector<std::string> &dst,
                          const rsx_dlss_extensions &source) {
    require(source.count<=RSX_DLSS_MAX_EXTENSIONS,"NGX extension count overflow");
    for(uint32_t i=0;i<source.count;i++) {
        std::string name=source.items[i].extensionName;
        if(std::find(dst.begin(),dst.end(),name)==dst.end())dst.push_back(name);
    }
}
static std::vector<const char*> pointers(const std::vector<std::string> &names) {
    std::vector<const char*> out;
    for(const auto &name:names)out.push_back(name.c_str());
    return out;
}
static bool extensionsPresent(const std::vector<std::string> &wanted,
                              const std::vector<VkExtensionProperties> &available) {
    for(const auto &name:wanted) {
        bool found=false;
        for(const auto &p:available)if(name==p.extensionName){found=true;break;}
        if(!found)return false;
    }
    return true;
}
static void initializeFG(App &a,const fs::path &logs,const fs::path &runtime,
                         const std::string &project,FGConsumer &consumer)
{
    a.logText=logs.wstring();a.runtimeText=runtime.wstring();
    auto id=fgIdentity(a,project);
    rsx_dlss_extensions sr{},fg{};rsx_dlss_fg_report report{};int32_t ngx=0;
    require(rsx_dlss_instance_extensions(&id,&sr,&ngx)==RSX_DLSS_OK,
            "SR instance extension discovery failed",77);
    rsx_dlss_status fgDiscovery = rsx_dlss_fg_instance_extensions(&id,&fg,&report);
    fgStatus(fgDiscovery,report,"instance extension discovery");
    std::vector<std::string> names;addExtensions(names,sr);addExtensions(names,fg);
    uint32_t count=0;
    vkcheck(vkEnumerateInstanceExtensionProperties(nullptr,&count,nullptr),
            "enumerate instance extensions");
    std::vector<VkExtensionProperties> available(count);
    vkcheck(vkEnumerateInstanceExtensionProperties(nullptr,&count,available.data()),
            "enumerate instance extensions");
    require(extensionsPresent(names,available),"required instance extension unavailable",77);
    auto enabled=pointers(names);
    VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName="Private DLSS FG moving fixture";app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ic{};ic.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ic.pApplicationInfo=&app;ic.enabledExtensionCount=static_cast<uint32_t>(enabled.size());
    ic.ppEnabledExtensionNames=enabled.data();
    vkcheck(vkCreateInstance(&ic,nullptr,&a.instance),"create instance");
    vkcheck(vkEnumeratePhysicalDevices(a.instance,&count,nullptr),"physical devices");
    require(count>0&&count<=256,"no bounded physical device inventory",77);
    std::vector<VkPhysicalDevice> physical(count);
    vkcheck(vkEnumeratePhysicalDevices(a.instance,&count,physical.data()),"physical devices");
    for(auto p:physical) {
        VkPhysicalDeviceProperties prop{};vkGetPhysicalDeviceProperties(p,&prop);
        if(prop.vendorID!=0x10de||prop.apiVersion<VK_API_VERSION_1_2)continue;
        uint32_t queues=0;vkGetPhysicalDeviceQueueFamilyProperties(p,&queues,nullptr);
        std::vector<VkQueueFamilyProperties> families(queues);
        vkGetPhysicalDeviceQueueFamilyProperties(p,&queues,families.data());
        uint32_t family=queues;
        for(uint32_t j=0;j<queues;j++)if(families[j].queueCount&&
            (families[j].queueFlags&(VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT))==
            (VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT)){family=j;break;}
        if(family==queues)continue;
        if(rsx_dlss_device_extensions(&id,a.instance,p,&sr,&ngx)!=RSX_DLSS_OK)
            continue;
        report={};
        if(rsx_dlss_fg_device_extensions(&id,a.instance,p,&fg,&report)!=RSX_DLSS_OK)
            continue;
        names.clear();addExtensions(names,sr);addExtensions(names,fg);
        uint32_t n=0;
        vkcheck(vkEnumerateDeviceExtensionProperties(p,nullptr,&n,nullptr),
                "enumerate device extensions");
        available.resize(n);
        vkcheck(vkEnumerateDeviceExtensionProperties(p,nullptr,&n,available.data()),
                "enumerate device extensions");
        if(!extensionsPresent(names,available))continue;
        a.physical=p;a.family=family;break;
    }
    require(a.physical!=VK_NULL_HANDLE,"no NVIDIA device with SR+FG extensions",77);
    enabled=pointers(names);
    VkPhysicalDeviceFeatures2 features{};features.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    VkPhysicalDeviceVulkan11Features f11{};f11.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};f12.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features.pNext=&f11;f11.pNext=&f12;vkGetPhysicalDeviceFeatures2(a.physical,&features);
    float priority=1;
    VkDeviceQueueCreateInfo qc{};qc.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qc.queueFamilyIndex=a.family;qc.queueCount=1;qc.pQueuePriorities=&priority;
    VkDeviceCreateInfo dc{};dc.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dc.pNext=&features;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;
    dc.enabledExtensionCount=static_cast<uint32_t>(enabled.size());
    dc.ppEnabledExtensionNames=enabled.data();
    vkcheck(vkCreateDevice(a.physical,&dc,nullptr,&a.device),"create device");
    vkGetDeviceQueue(a.device,a.family,0,&a.queue);
    vkGetPhysicalDeviceMemoryProperties(a.physical,&a.memory);
    rsx_dlss_device device{a.instance,a.physical,a.device,vkGetInstanceProcAddr,vkGetDeviceProcAddr};
    rsx_dlss_report srReport{};
    require(rsx_dlss_create(&id,&device,&consumer.value,&srReport)==RSX_DLSS_OK,
            "NGX context/SR capability unavailable",77);
    consumer.device=a.device;
    rsx_dlss_status fgCapability=rsx_dlss_fg_capability(consumer.value,&report);
    fgStatus(fgCapability,report,"capability");
    VkCommandPoolCreateInfo pc{};pc.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pc.queueFamilyIndex=a.family;
    vkcheck(vkCreateCommandPool(a.device,&pc,nullptr,&a.pool),"command pool");
    VkCommandBufferAllocateInfo bc{};bc.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    bc.commandPool=a.pool;bc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;bc.commandBufferCount=1;
    vkcheck(vkAllocateCommandBuffers(a.device,&bc,&a.command),"command buffer");
    VkFenceCreateInfo fc{};fc.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkcheck(vkCreateFence(a.device,&fc,nullptr,&a.fence),"fence");
}
static rsx_dlss_image fgImage(const Image &i,VkImageLayout layout) {
    return {i.image,i.view,i.format,i.width,i.height,
            {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},layout};
}
struct FGPixel {Pixel pixel;bool object;};
static FGPixel fgPixel(float x,float y,uint32_t frame) {
    const float camera=.003f*frame;
    const float wx=x+camera;
    const float cx=.35f+.007f*frame,cy=.48f+.003f*frame;
    const float dx=wx-cx,dy=y-cy;
    const bool object=dx*dx+dy*dy<.14f*.14f;
    const bool grid=(static_cast<int>(std::floor(wx*24))+
                     static_cast<int>(std::floor(y*16)))%2!=0;
    std::array<float,4> color={grid?.10f:.25f,.15f+.5f*wx,.10f+.45f*y,1};
    if(object)color={.80f,.10f+.25f*dx,.15f+.25f*dy,1};
    const float z=object?2.0f:10.0f;
    return {{color,100.0f/99.9f-10.0f/(99.9f*z)},object};
}
static void identity4(float *v) {
    std::fill(v,v+16,0.0f);for(unsigned i=0;i<4;i++)v[i*4+i]=1;
}
static void cameraForFrame(rsx_dlss_fg_camera &c,uint32_t frame,
                            uint32_t width,uint32_t height) {
    identity4(c.view_to_clip);identity4(c.clip_to_view);
    identity4(c.clip_to_lens_clip);identity4(c.clip_to_prev_clip);
    identity4(c.prev_clip_to_clip);
    const float sx=static_cast<float>(height)/width,sy=1.0f;
    const float nearPlane=.1f,farPlane=100.0f;
    const float A=farPlane/(farPlane-nearPlane),B=-farPlane*nearPlane/(farPlane-nearPlane);
    const float shift=.003f*frame;
    std::fill(c.view_to_clip,c.view_to_clip+16,0.0f);
    c.view_to_clip[0]=sx;c.view_to_clip[5]=sy;
    c.view_to_clip[11]=B;c.view_to_clip[10]=A;c.view_to_clip[14]=1;
    c.view_to_clip[2]=-2*shift;
    std::fill(c.clip_to_view,c.clip_to_view+16,0.0f);
    c.clip_to_view[0]=1/sx;c.clip_to_view[5]=1/sy;
    c.clip_to_view[3]=2*shift/sx;
    c.clip_to_view[11]=1;c.clip_to_view[14]=1/B;c.clip_to_view[15]=-A/B;
    const float delta=.003f; /* Frame zero uses a virtual prior pose; reset ignores it. */
    c.clip_to_prev_clip[3]=2*delta;
    c.prev_clip_to_clip[3]=-2*delta;
    c.position[0]=c.position[1]=c.position[2]=0;
    c.right[0]=1;c.right[1]=c.right[2]=0;
    c.up[1]=1;c.up[0]=c.up[2]=0;
    c.forward[2]=1;c.forward[0]=c.forward[1]=0;
    c.near_plane=nearPlane;c.far_plane=farPlane;c.fov_radians=1.5707963f;
    c.aspect_ratio=static_cast<float>(width)/height;
    c.jitter_clip_x=0;c.jitter_clip_y=0;
}
static void rejectFG(rsx_dlss_vk *c,VkCommandBuffer command,
                     const rsx_dlss_fg_frame &f,const char *label) {
    require(rsx_dlss_fg_evaluate(c,command,&f)==RSX_DLSS_BAD_ARGUMENT,label,1);
}
static void runFG(App &a,FGConsumer &consumer,HUDImage &hudless,
                  const fs::path &prefix,uint32_t width,uint32_t height,
                  uint32_t frames)
{
    const size_t pixels=static_cast<size_t>(width)*height;
    const uint32_t roiYBegin=height/4,roiYEnd=(3*height)/4;
    const size_t roiPixels=static_cast<size_t>(width)*(roiYEnd-roiYBegin);
    require(roiPixels>0,"empty FG comparison region",2);
    rsx_dlss_fg_config cfg{1,width,height,width,height,0};
    FGFlagBuffer suppression;suppression.device=a.device;
    a.begin();
    rsx_dlss_status status=rsx_dlss_fg_configure(consumer.value,a.command,&cfg);
    rsx_dlss_fg_report report{};rsx_dlss_fg_get_report(consumer.value,&report);
    fgStatus(status,report,"configure");
    a.finish();
    a.makeImage(a.images[0],width,height,VK_FORMAT_R16G16B16A16_SFLOAT);
    hudless.device=a.device;
    a.makeImage(hudless.value,width,height,VK_FORMAT_R16G16B16A16_SFLOAT);
    a.makeImage(a.images[1],width,height,VK_FORMAT_R32_SFLOAT);
    a.makeImage(a.images[2],width,height,VK_FORMAT_R32G32_SFLOAT);
    a.makeImage(a.images[3],width,height,VK_FORMAT_R16G16B16A16_SFLOAT);
    const VkDeviceSize colorBytes=pixels*8,depthBytes=pixels*4;
    const VkDeviceSize depthOffset=colorBytes*2;
    const VkDeviceSize motionOffset=(depthOffset+depthBytes+7)&~VkDeviceSize(7);
    a.makeBuffer(a.upload,motionOffset+pixels*8,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    a.makeBuffer(a.readback,colorBytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    a.makeBuffer(suppression.value,4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    rsx_dlss_fg_frame f{};
    f.config_generation=cfg.generation;f.frame_serial=1;f.motion_frame_serial=1;
    f.motion_provenance=RSX_DLSS_MOTION_CAMERA_AND_OBJECT_UNJITTERED;
    f.backbuffer=fgImage(a.images[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    f.hudless=fgImage(hudless.value,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    f.depth=fgImage(a.images[1],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    f.motion=fgImage(a.images[2],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    f.output_interpolated=fgImage(a.images[3],VK_IMAGE_LAYOUT_GENERAL);
    f.output_disable_interpolation={suppression.value.buffer,4,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    cameraForFrame(f.camera,0,width,height);f.reset=1;
    rsx_dlss_fg_frame bad=f;bad.motion_provenance=0;
    rejectFG(consumer.value,a.command,bad,"FG accepted missing motion provenance");
    bad=f;bad.motion_frame_serial=0;
    rejectFG(consumer.value,a.command,bad,"FG accepted stale motion serial");
    bad=f;bad.frame_serial=0;bad.motion_frame_serial=0;
    rejectFG(consumer.value,a.command,bad,"FG accepted zero frame serial");
    bad=f;bad.config_generation++;
    rejectFG(consumer.value,a.command,bad,"FG accepted wrong config generation");
    bad=f;bad.output_interpolated.image=f.backbuffer.image;
    rejectFG(consumer.value,a.command,bad,"FG accepted output alias");
    bad=f;bad.output_disable_interpolation.size_bytes=3;
    rejectFG(consumer.value,a.command,bad,"FG accepted short suppression buffer");
    std::vector<float> first(pixels*4),last(pixels*4),lastInput(pixels*4),
        lastPreviousInput(pixels*4),currentInput(pixels*4),previousInput(pixels*4);
    uint32_t suppressionCount=0,validCount=0,distinctCount=0;
    double lastMeanCurrent=0,lastMeanPrevious=0,bestMinMean=0;
    // The dev DLL stamps an FG watermark near the top. Compare only middle
    // rows, excluding that overlay; inputs are already half-quantized.
    // Mean RGB difference 0.0005 rejects simple copies in this moving scene
    // without asserting interpolation accuracy.
    constexpr double passthroughTolerance=0.0005;
    for(uint32_t frame=0;frame<frames;frame++) {
        auto bytes=static_cast<unsigned char*>(a.upload.mapped);
        auto back=reinterpret_cast<uint16_t*>(bytes);
        auto hud=reinterpret_cast<uint16_t*>(bytes+colorBytes);
        auto depth=reinterpret_cast<float*>(bytes+depthOffset);
        auto motion=reinterpret_cast<float*>(bytes+motionOffset);
        for(uint32_t y=0;y<height;y++)for(uint32_t x=0;x<width;x++) {
            const float sx=(static_cast<float>(x)+.5f)/width;
            const float sy=(static_cast<float>(y)+.5f)/height;
            const FGPixel sample=fgPixel(sx,sy,frame);
            const size_t i=static_cast<size_t>(y)*width+x;
            depth[i]=sample.pixel.depth;
            for(unsigned k=0;k<4;k++) {
                back[i*4+k]=half(sample.pixel.color[k]);
                hud[i*4+k]=back[i*4+k];
                currentInput[i*4+k]=unhalf(back[i*4+k]);
            }
            motion[i*2]=(sample.object?(.003f-.007f)*width:.003f*width);
            motion[i*2+1]=sample.object?-.003f*height:0;
        }
        f.frame_serial=frame+1;f.motion_frame_serial=f.frame_serial;
        f.reset=frame==0;cameraForFrame(f.camera,frame,width,height);
        a.begin();
        vkCmdFillBuffer(a.command,suppression.value.buffer,0,4,0);
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
        host.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&host,0,nullptr,0,nullptr);
        Image *inputs[4]={&a.images[0],&hudless.value,&a.images[1],&a.images[2]};
        VkDeviceSize offsets[4]={0,colorBytes,depthOffset,motionOffset};
        for(unsigned k=0;k<4;k++) {
            a.transition(*inputs[k],frame?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                         VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         frame?VK_ACCESS_SHADER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT,
                         frame?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};copy.bufferOffset=offsets[k];
            copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            copy.imageExtent={width,height,1};
            vkCmdCopyBufferToImage(a.command,a.upload.buffer,inputs[k]->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            a.transition(*inputs[k],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        a.transition(a.images[3],frame?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                     VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,
                     frame?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT,
                     frame?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue clear{};clear.float32[0]=1;clear.float32[2]=1;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(a.command,a.images[3].image,VK_IMAGE_LAYOUT_GENERAL,
                             &clear,1,&range);
        VkMemoryBarrier cleared{};cleared.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        cleared.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        cleared.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&cleared,0,nullptr,0,nullptr);
        status=rsx_dlss_fg_evaluate(consumer.value,a.command,&f);
        rsx_dlss_fg_get_report(consumer.value,&report);
        fgStatus(status,report,"evaluate");
        a.transition(a.images[3],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageExtent={width,height,1};
        vkCmdCopyImageToBuffer(a.command,a.images[3].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               a.readback.buffer,1,&copy);
        VkMemoryBarrier read{};read.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        read.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        read.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TRANSFER_BIT|
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT,0,1,&read,0,nullptr,0,nullptr);
        a.finish();
        if(frame==2) {
            bad=f;
            rejectFG(consumer.value,a.command,bad,"FG accepted repeated real-frame serial");
        }
        const bool disabled=*static_cast<const uint8_t*>(suppression.value.mapped)!=0;
        if(disabled)++suppressionCount;
        if(frame>=1&&!disabled) {
            auto raw=static_cast<const uint16_t*>(a.readback.mapped);
            for(size_t i=0;i<last.size();i++) {
                last[i]=unhalf(raw[i]);
                require(std::isfinite(last[i]),"nonfinite FG readback",1);
            }
            double deltaCurrent=0,deltaPrevious=0;
            for(uint32_t y=roiYBegin;y<roiYEnd;y++)
                for(uint32_t x=0;x<width;x++) {
                    const size_t i=(static_cast<size_t>(y)*width+x)*4;
                    for(unsigned k=0;k<3;k++) {
                        deltaCurrent+=std::fabs(last[i+k]-currentInput[i+k]);
                        deltaPrevious+=std::fabs(last[i+k]-previousInput[i+k]);
                    }
                }
            lastMeanCurrent=deltaCurrent/(roiPixels*3);
            lastMeanPrevious=deltaPrevious/(roiPixels*3);
            const double minMean=std::min(lastMeanCurrent,lastMeanPrevious);
            bestMinMean=std::max(bestMinMean,minMean);
            if(minMean>passthroughTolerance)++distinctCount;
            if(validCount++==0)first=last;
            lastInput=currentInput;
            lastPreviousInput=previousInput;
        }
        previousInput=currentInput;
    }
    std::printf("FG suppression: %u of %u recorded frames; usable outputs=%u\n",
                suppressionCount,frames,validCount);
    std::printf("FG real-frame ROI: rows [%u,%u), %zu pixels, %zu RGB samples\n",
                roiYBegin,roiYEnd,roiPixels,roiPixels*3);
    std::printf("FG real-frame ROI deltas: last/current=%.9g last/previous=%.9g; "
                "best minimum=%.9g; distinct=%u; threshold=%.6g\n",
                lastMeanCurrent,lastMeanPrevious,bestMinMean,distinctCount,
                passthroughTolerance);
    require(validCount>=2,"fewer than two unsuppressed FG outputs",77);
    require(distinctCount>=1,
            "FG outputs indistinguishable from previous/current real-frame passthrough",1);
    double nonSentinel=0,temporalChange=0;
    for(size_t i=0;i<last.size();i+=4) {
        nonSentinel+=std::fabs(last[i]-1)+std::fabs(last[i+1])+std::fabs(last[i+2]-1);
        for(unsigned k=0;k<3;k++)temporalChange+=std::fabs(last[i+k]-first[i+k]);
    }
    require(nonSentinel/pixels>0.001,"FG output remained clear sentinel",1);
    require(temporalChange/(pixels*3)>1e-6,"FG output did not change over moving frames",1);
    require(report.record_count==frames&&report.last_frame_serial==frames,
            "FG recorded count/serial mismatch",1);
    writePPM(prefix.wstring()+L".fg-first.ppm",first,width,height);
    writePPM(prefix.wstring()+L".fg-last.ppm",last,width,height);
    writePPM(prefix.wstring()+L".real-previous.ppm",lastPreviousInput,width,height);
    writePPM(prefix.wstring()+L".real-last.ppm",lastInput,width,height);
    FILE *file=nullptr;
    require(_wfopen_s(&file,(prefix.wstring()+L".fg-last.rgba32f").c_str(),L"wb")==0,
            "open FG raw output",1);
    bool written=std::fwrite(last.data(),sizeof(float),last.size(),file)==last.size();
    if(std::fclose(file))written=false;
    require(written,"write FG raw output",1);
    std::printf("FG fixture: %u moving frames, %ux%u, usable outputs %u, output delta %.9g\n",
                 frames,width,height,validCount,temporalChange/(pixels*3));
    std::puts("GPU readback only; no presentation, gameplay, quality or FPS claim.");
}
int wmain(int argc,wchar_t **argv) {
    try {
        if(argc!=8) {std::fputs("Usage: test_vulkan_dlss_fg_consumer OUTPUT_PREFIX LOG_DIR RUNTIME_DIR PROJECT_UUID WIDTH HEIGHT FRAMES\n",stderr);return 2;}
        const fs::path prefix=fs::absolute(argv[1]),logs=fs::absolute(argv[2]);
        const fs::path runtime=fs::absolute(argv[3]);const std::string project=uuid(argv[4]);
        require(fs::is_directory(prefix.parent_path()),"output parent must exist",2);
        require(fs::is_directory(runtime)&&
                fs::is_regular_file(runtime/L"nvngx_dlss.dll")&&
                fs::is_regular_file(runtime/L"nvngx_dlssg.dll"),
                "explicit runtime must contain SR and FG DLLs",77);
        fs::create_directories(logs);
        uint32_t width=number(argv[5],64,2048),height=number(argv[6],64,2048);
        uint32_t frames=number(argv[7],4,32);
        App app;FGConsumer consumer;HUDImage hudless;
        initializeFG(app,logs,runtime,project,consumer);
        runFG(app,consumer,hudless,prefix,width,height,frames);
        return 0;
    } catch(const Failure &f) {
        std::fprintf(stderr,"%s: %s\n",f.code==77?"UNSUPPORTED":"FAILED",f.message.c_str());
        return f.code;
    } catch(const std::exception &e) {
        std::fprintf(stderr,"FAILED: %s\n",e.what());return 1;
    }
}

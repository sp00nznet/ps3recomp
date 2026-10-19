/* Private GPU diagnostic for the rsx_dlss_vk consumer, not a game test.
 * Build with MSVC /std:c++17 /MD /Gy and /OPT:REF. Include this SDK's
 * libs/video, the Vulkan headers and the official NGX Vulkan headers; link
 * rsx_dlss_vk.cpp, vulkan-1.lib and nvsdk_ngx_d.lib. The latter is also used
 * by the reused fixture translation unit below; never invoke its old wmain.
 * Usage: test_vulkan_dlss_consumer OUTPUT_PREFIX LOG_DIR RUNTIME_DIR
 *        PROJECT_UUID WIDTH HEIGHT FRAMES (FRAMES >= 16)
 */
#define wmain old_dlss_sr_wmain_never_call
#include "test_vulkan_dlss_sr.cpp"
#undef wmain
#include "../libs/video/rsx_dlss_vk.h"

static void consumerCheck(rsx_dlss_status s,const char *step,int code=1) {
    if(s!=RSX_DLSS_OK)throw Failure{code,std::string(step)+": consumer status="+std::to_string(s)};
}
static rsx_dlss_identity identity(const App &a,const std::string &project) {
    return {project.c_str(),reinterpret_cast<const uint16_t*>(a.logText.c_str()),
            reinterpret_cast<const uint16_t*>(a.runtimeText.c_str())};
}
struct Consumer {
    rsx_dlss_vk *value=nullptr;
    VkDevice device=VK_NULL_HANDLE;
    ~Consumer(){if(value){if(device)vkDeviceWaitIdle(device);rsx_dlss_destroy(value);}}
};
static std::vector<const char*> consumerNames(const rsx_dlss_extensions &list) {
    require(list.count<=RSX_DLSS_MAX_EXTENSIONS,"consumer extension count overflow");
    std::vector<const char*> out;
    for(uint32_t i=0;i<list.count;i++)out.push_back(list.items[i].extensionName);
    return out;
}
static void initializeConsumer(App &a,const fs::path &logs,
                               const fs::path &runtime,const std::string &project,
                               Consumer &consumer)
{
    a.logText=logs.wstring();a.runtimeText=runtime.wstring();
    auto id=identity(a,project);
    rsx_dlss_extensions required{};int32_t ngx=0;
    consumerCheck(rsx_dlss_instance_extensions(&id,&required,&ngx),
                  "instance extension discovery",77);
    auto names=consumerNames(required);
    uint32_t count=0;
    vkcheck(vkEnumerateInstanceExtensionProperties(nullptr,&count,nullptr),"instance extensions");
    std::vector<VkExtensionProperties> available(count);
    vkcheck(vkEnumerateInstanceExtensionProperties(nullptr,&count,available.data()),"instance extensions");
    verifyExtensions(names,available);
    VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName="DLSS consumer moving fixture";app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ic{};ic.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ic.pApplicationInfo=&app;ic.enabledExtensionCount=static_cast<uint32_t>(names.size());
    ic.ppEnabledExtensionNames=names.data();
    vkcheck(vkCreateInstance(&ic,nullptr,&a.instance),"create instance");
    vkcheck(vkEnumeratePhysicalDevices(a.instance,&count,nullptr),"physical devices");
    require(count>0 && count<=256,"no bounded physical device inventory",77);
    std::vector<VkPhysicalDevice> physical(count);
    vkcheck(vkEnumeratePhysicalDevices(a.instance,&count,physical.data()),"physical devices");
    for(auto p:physical) {
        VkPhysicalDeviceProperties prop{};vkGetPhysicalDeviceProperties(p,&prop);
        if(prop.vendorID!=0x10de || prop.apiVersion<VK_API_VERSION_1_2)continue;
        uint32_t queues=0;vkGetPhysicalDeviceQueueFamilyProperties(p,&queues,nullptr);
        std::vector<VkQueueFamilyProperties> families(queues);
        vkGetPhysicalDeviceQueueFamilyProperties(p,&queues,families.data());
        uint32_t family=queues;
        for(uint32_t j=0;j<queues;j++)if(families[j].queueCount &&
            (families[j].queueFlags&(VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT))==
            (VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT)){family=j;break;}
        if(family==queues)continue;
        auto status=rsx_dlss_device_extensions(&id,a.instance,p,&required,&ngx);
        if(status!=RSX_DLSS_OK)continue;
        names=consumerNames(required);
        uint32_t n=0;
        vkcheck(vkEnumerateDeviceExtensionProperties(p,nullptr,&n,nullptr),"device extensions");
        available.resize(n);
        vkcheck(vkEnumerateDeviceExtensionProperties(p,nullptr,&n,available.data()),"device extensions");
        verifyExtensions(names,available);
        a.physical=p;a.family=family;break;
    }
    require(a.physical!=VK_NULL_HANDLE,"no NVIDIA device supports consumer discovery",77);
    VkPhysicalDeviceFeatures2 features{};features.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    VkPhysicalDeviceVulkan11Features f11{};f11.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};f12.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features.pNext=&f11;f11.pNext=&f12;vkGetPhysicalDeviceFeatures2(a.physical,&features);
    float priority=1;
    VkDeviceQueueCreateInfo qc{};qc.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qc.queueFamilyIndex=a.family;qc.queueCount=1;qc.pQueuePriorities=&priority;
    VkDeviceCreateInfo dc{};dc.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dc.pNext=&features;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;
    dc.enabledExtensionCount=static_cast<uint32_t>(names.size());dc.ppEnabledExtensionNames=names.data();
    vkcheck(vkCreateDevice(a.physical,&dc,nullptr,&a.device),"create device");
    vkGetDeviceQueue(a.device,a.family,0,&a.queue);
    vkGetPhysicalDeviceMemoryProperties(a.physical,&a.memory);
    rsx_dlss_device device{a.instance,a.physical,a.device,vkGetInstanceProcAddr,vkGetDeviceProcAddr};
    rsx_dlss_report report{};
    consumerCheck(rsx_dlss_create(&id,&device,&consumer.value,&report),"create consumer",77);
    consumer.device=a.device;
    require(report.available && !report.needs_driver_update,"DLSS unavailable",77);
    VkCommandPoolCreateInfo pc{};pc.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pc.queueFamilyIndex=a.family;
    vkcheck(vkCreateCommandPool(a.device,&pc,nullptr,&a.pool),"create command pool");
    VkCommandBufferAllocateInfo bc{};bc.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    bc.commandPool=a.pool;bc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;bc.commandBufferCount=1;
    vkcheck(vkAllocateCommandBuffers(a.device,&bc,&a.command),"allocate commands");
    VkFenceCreateInfo fc{};fc.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkcheck(vkCreateFence(a.device,&fc,nullptr,&a.fence),"create fence");
}
static rsx_dlss_image frameImage(const Image &i,VkImageLayout layout) {
    return {i.image,i.view,i.format,i.width,i.height,
            {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},layout};
}
struct MovingPixel {Pixel pixel;bool object;};
static MovingPixel movingPixel(float x,float y,uint32_t frame) {
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
static void expectRejected(rsx_dlss_vk *consumer,VkCommandBuffer command,
                           const rsx_dlss_frame &frame,const char *label) {
    require(rsx_dlss_evaluate(consumer,command,&frame)==RSX_DLSS_BAD_ARGUMENT,label,1);
}
static void runMoving(App &a,Consumer &consumer,const fs::path &prefix,
                      uint32_t width,uint32_t height,uint32_t frames)
{
    rsx_dlss_size size{};
    consumerCheck(rsx_dlss_optimal_size(consumer.value,width,height,RSX_DLSS_QUALITY,&size),
                  "optimal size",77);
    const uint32_t iw=size.input_width,ih=size.input_height;
    require(iw && ih && iw<=width && ih<=height,"invalid input size",77);
    rsx_dlss_config cfg{1,iw,ih,width,height,RSX_DLSS_QUALITY,1};
    a.begin();consumerCheck(rsx_dlss_configure(consumer.value,a.command,&cfg),"configure",77);
    a.finish();
    a.makeImage(a.images[0],iw,ih,VK_FORMAT_R16G16B16A16_SFLOAT);
    a.makeImage(a.images[1],iw,ih,VK_FORMAT_R32_SFLOAT);
    a.makeImage(a.images[2],iw,ih,VK_FORMAT_R32G32_SFLOAT);
    a.makeImage(a.images[3],width,height,VK_FORMAT_R16G16B16A16_SFLOAT);
    const size_t inputCount=static_cast<size_t>(iw)*ih,outputCount=static_cast<size_t>(width)*height;
    const VkDeviceSize colorBytes=inputCount*8,depthBytes=inputCount*4;
    const VkDeviceSize motionOffset=(colorBytes+depthBytes+7)&~VkDeviceSize(7);
    a.makeBuffer(a.upload,motionOffset+inputCount*8,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    a.makeBuffer(a.readback,outputCount*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    rsx_dlss_frame f{};
    f.config_generation=cfg.generation;f.frame_serial=1;f.motion_frame_serial=1;
    f.motion_provenance=RSX_DLSS_MOTION_CAMERA_AND_OBJECT_UNJITTERED;
    f.color=frameImage(a.images[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    f.depth=frameImage(a.images[1],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    f.motion=frameImage(a.images[2],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    f.output=frameImage(a.images[3],VK_IMAGE_LAYOUT_GENERAL);
    f.delta_milliseconds=1000.0f/60;f.pre_exposure=1;f.exposure_scale=1;f.reset=1;
    rsx_dlss_frame bad=f;bad.motion_provenance=0;
    expectRejected(consumer.value,a.command,bad,"missing motion provenance accepted");
    bad=f;bad.motion_frame_serial=0;
    expectRejected(consumer.value,a.command,bad,"stale motion serial accepted");
    bad=f;bad.frame_serial=0;bad.motion_frame_serial=0;
    expectRejected(consumer.value,a.command,bad,"zero frame serial accepted");
    bad=f;bad.config_generation++;
    expectRejected(consumer.value,a.command,bad,"stale config generation accepted");
    bad=f;bad.motion.width++;
    expectRejected(consumer.value,a.command,bad,"mismatched motion extent accepted");
    bad=f;bad.output.height++;
    expectRejected(consumer.value,a.command,bad,"mismatched output extent accepted");
    rsx_dlss_report report{};consumerCheck(rsx_dlss_get_report(consumer.value,&report),"report");
    require(report.evaluation_count==0,"rejected frame reached NGX",1);
    std::vector<float> first(outputCount*4),last(outputCount*4);
    for(uint32_t frame=0;frame<frames;frame++) {
        const float jx=halton(frame+1,2)-.5f,jy=halton(frame+1,3)-.5f;
        auto bytes=static_cast<unsigned char*>(a.upload.mapped);
        auto color=reinterpret_cast<uint16_t*>(bytes);
        auto depth=reinterpret_cast<float*>(bytes+colorBytes);
        auto motion=reinterpret_cast<float*>(bytes+motionOffset);
        for(uint32_t y=0;y<ih;y++)for(uint32_t x=0;x<iw;x++) {
            const float sx=(static_cast<float>(x)+.5f-jx)/iw;
            const float sy=(static_cast<float>(y)+.5f-jy)/ih;
            const MovingPixel sample=movingPixel(sx,sy,frame);
            const size_t i=static_cast<size_t>(y)*iw+x;
            depth[i]=sample.pixel.depth;
            for(unsigned c=0;c<4;c++)color[i*4+c]=half(sample.pixel.color[c]);
            // Previous minus current positions in unjittered input pixels.
            motion[i*2]=frame?(sample.object?(.003f-.007f)*iw:.003f*iw):0;
            motion[i*2+1]=frame&&sample.object?-.003f*ih:0;
        }
        f.frame_serial=frame+1;f.motion_frame_serial=f.frame_serial;
        f.jitter_x=jx;f.jitter_y=jy;f.reset=frame==0;
        a.begin();
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,1,&host,0,nullptr,0,nullptr);
        for(size_t i=0;i<3;i++) {
            a.transition(a.images[i],frame?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,frame?VK_ACCESS_SHADER_READ_BIT:0,
                         VK_ACCESS_TRANSFER_WRITE_BIT,frame?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};copy.bufferOffset=i==0?0:i==1?colorBytes:motionOffset;
            copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={iw,ih,1};
            vkCmdCopyBufferToImage(a.command,a.upload.buffer,a.images[i].image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            a.transition(a.images[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        a.transition(a.images[3],frame?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                     VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,
                     frame?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT,
                     frame?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue clear{};clear.float32[3]=1;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(a.command,a.images[3].image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
        VkMemoryBarrier clearBarrier{};clearBarrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        clearBarrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&clearBarrier,0,nullptr,0,nullptr);
        consumerCheck(rsx_dlss_evaluate(consumer.value,a.command,&f),"evaluate");
        a.transition(a.images[3],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageExtent={width,height,1};
        vkCmdCopyImageToBuffer(a.command,a.images[3].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               a.readback.buffer,1,&copy);
        VkMemoryBarrier read{};read.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        read.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;read.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                             0,1,&read,0,nullptr,0,nullptr);
        a.finish();
        auto raw=static_cast<const uint16_t*>(a.readback.mapped);
        for(size_t i=0;i<last.size();i++) {
            last[i]=unhalf(raw[i]);
            require(std::isfinite(last[i]),"nonfinite consumer RGBA readback",1);
        }
        if(frame==0)first=last;
        if(frame==1) {
            bad=f;bad.frame_serial=1;bad.motion_frame_serial=1;
            expectRejected(consumer.value,a.command,bad,"repeated frame serial accepted");
        }
    }
    double change=0;
    for(size_t i=0;i<last.size();i+=4)for(unsigned c=0;c<3;c++)
        change+=std::fabs(last[i+c]-first[i+c]);
    change/=outputCount*3;
    require(change>1e-5,"moving fixture produced no temporal RGB change",1);
    consumerCheck(rsx_dlss_get_report(consumer.value,&report),"final report");
    require(report.evaluation_count==frames && report.last_frame_serial==frames,
            "consumer evaluation count/serial mismatch",1);
    writePPM(prefix.wstring()+L".dlss.ppm",last,width,height);
    FILE *raw=nullptr;
    require(_wfopen_s(&raw,(prefix.wstring()+L".dlss.rgba32f").c_str(),L"wb")==0,
            "open raw output failed",1);
    bool written=std::fwrite(last.data(),sizeof(float),last.size(),raw)==last.size();
    if(std::fclose(raw))written=false;
    require(written,"raw output write failed",1);
    std::printf("consumer: %u moving frames, input %ux%u, output %ux%u, temporal RGB delta %.9g\n",
                frames,iw,ih,width,height,change);
    std::puts("Synthetic GPU consumer evidence only; no gameplay or visual-improvement claim.");
}
int wmain(int argc,wchar_t **argv) {
    try {
        if(argc!=8) {std::fputs("Usage: test_vulkan_dlss_consumer OUTPUT_PREFIX LOG_DIR RUNTIME_DIR PROJECT_UUID WIDTH HEIGHT FRAMES\n",stderr);return 2;}
        const fs::path prefix=fs::absolute(argv[1]),logs=fs::absolute(argv[2]);
        const fs::path runtime=fs::absolute(argv[3]);const std::string project=uuid(argv[4]);
        require(fs::is_directory(prefix.parent_path()),"output parent must exist",2);
        require(fs::is_directory(runtime) && fs::is_regular_file(runtime/L"nvngx_dlss.dll"),
                "explicit runtime must contain nvngx_dlss.dll",2);
        fs::create_directories(logs);
        const uint32_t width=number(argv[5],64,2048),height=number(argv[6],64,2048);
        const uint32_t frames=number(argv[7],16,64);
        App app;Consumer consumer;
        initializeConsumer(app,logs,runtime,project,consumer);
        runMoving(app,consumer,prefix,width,height,frames);
        return 0;
    } catch(const Failure &f) {
        std::fprintf(stderr,"%s: %s\n",f.code==77?"UNAVAILABLE":"FAILED",f.message.c_str());
        return f.code;
    } catch(const std::exception &e) {
        std::fprintf(stderr,"FAILED: %s\n",e.what());return 1;
    }
}

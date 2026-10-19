/* Standalone Vulkan primary-ray geometry validator. Exit 77 means unavailable.
 * No presentation, shared device, emulator state, or driver settings are used.
 * Depth is little-endian float32 view Z; normals PPM maps [-1,1] to [0,255].
 * Optional --ao emits .ao.f32le (occluded fraction 0..1) and .ao.ppm
 * (visibility: white=open, black=occluded). Primary misses are float 0 and black.
 * Offline captured geometry only: opaque double-sided triangles, no alpha,
 * materials, transmission, dynamic gameplay state, or lighting reconstruction.
 * AO uses deterministic Hammersley cosine-weighted hemisphere samples:
 * u=(i+.5)/N, phi=2*pi*radicalInverseBase2(i), local z=sqrt(1-u).
 * Geometric normal faces the camera; origin offset=bias*normal, distance is
 * measured from that offset in scene units along normalized secondary rays.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <errno.h>

typedef struct Buffer { VkBuffer b; VkDeviceMemory m; void *map; VkDeviceAddress address; } Buffer;
static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory;
static VkQueue queue;
static uint32_t family;
static PFN_vkCreateAccelerationStructureKHR createAS;
static PFN_vkDestroyAccelerationStructureKHR destroyAS;
static PFN_vkGetAccelerationStructureBuildSizesKHR getSizes;
static PFN_vkCmdBuildAccelerationStructuresKHR buildAS;
static PFN_vkGetAccelerationStructureDeviceAddressKHR getASAddress;
static int check(VkResult r,const char *what) {
    if(r==VK_SUCCESS) return 1;
    fprintf(stderr,"%s: VkResult %d\n",what,(int)r); return 0;
}
static void freeBuffer(Buffer *b) {
    if(b->map) vkUnmapMemory(device,b->m);
    if(b->b) vkDestroyBuffer(device,b->b,NULL);
    if(b->m) vkFreeMemory(device,b->m,NULL);
    memset(b,0,sizeof(*b));
}
static int buffer(Buffer *b,VkDeviceSize size,VkBufferUsageFlags usage,int host) {
    VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(!check(vkCreateBuffer(device,&ci,NULL,&b->b),"create buffer")) return 0;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,b->b,&req);
    VkMemoryPropertyFlags flags=host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<memory.memoryTypeCount;i++) if((req.memoryTypeBits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags) {type=i;break;}
    if(type==UINT32_MAX) {fprintf(stderr,"required memory type unavailable\n");return 0;}
    VkMemoryAllocateFlagsInfo af={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,.flags=(usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)?VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT:0};
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&af,.allocationSize=req.size,.memoryTypeIndex=type};
    if(!check(vkAllocateMemory(device,&ai,NULL,&b->m),"allocate buffer") || !check(vkBindBufferMemory(device,b->b,b->m,0),"bind buffer")) return 0;
    if(host && !check(vkMapMemory(device,b->m,0,VK_WHOLE_SIZE,0,&b->map),"map buffer")) return 0;
    if(usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo di={.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=b->b};
        b->address=vkGetBufferDeviceAddress(device,&di);
        if(!b->address) {fprintf(stderr,"zero buffer device address\n");return 0;}
    }
    return 1;
}
static int hasExtension(VkPhysicalDevice p,const char *name) {
    uint32_t count=0;
    if(vkEnumerateDeviceExtensionProperties(p,NULL,&count,NULL)!=VK_SUCCESS || count>65536) return 0;
    VkExtensionProperties *e=calloc(count,sizeof(*e)); if(!e) return 0;
    int found=0;
    if(vkEnumerateDeviceExtensionProperties(p,NULL,&count,e)==VK_SUCCESS)
        for(uint32_t i=0;i<count;i++) if(!strcmp(e[i].extensionName,name)) found=1;
    free(e);return found;
}
static const char *extensions[]={VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,VK_KHR_RAY_QUERY_EXTENSION_NAME,VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
static int initialize(VkPhysicalDeviceAccelerationStructurePropertiesKHR *asProps) {
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="geometry ray query validator",.apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    if(!check(vkCreateInstance(&ci,NULL,&instance),"create Vulkan 1.2 instance")) return 0;
    uint32_t count=0;
    if(vkEnumeratePhysicalDevices(instance,&count,NULL)!=VK_SUCCESS || !count || count>256) return 0;
    VkPhysicalDevice *devices=calloc(count,sizeof(*devices));if(!devices) return 0;
    if(vkEnumeratePhysicalDevices(instance,&count,devices)!=VK_SUCCESS) {free(devices);return 0;}
    for(uint32_t i=0;i<count && !physical;i++) {
        VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(devices[i],&props);
        if(props.apiVersion<VK_API_VERSION_1_2) continue;
        int supported=1;for(unsigned j=0;j<3;j++) if(!hasExtension(devices[i],extensions[j])) supported=0;
        if(!supported) continue;
        VkPhysicalDeviceRayQueryFeaturesKHR rq={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,.pNext=&rq};
        VkPhysicalDeviceVulkan12Features v12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&as};
        VkPhysicalDeviceFeatures2 f={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&v12};
        vkGetPhysicalDeviceFeatures2(devices[i],&f);
        if(!rq.rayQuery || !as.accelerationStructure || !v12.bufferDeviceAddress) continue;
        VkFormatProperties fmt;vkGetPhysicalDeviceFormatProperties(devices[i],VK_FORMAT_R32G32B32_SFLOAT,&fmt);
        if(!(fmt.bufferFeatures&VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR)) continue;
        uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(devices[i],&n,NULL);
        VkQueueFamilyProperties *qp=calloc(n,sizeof(*qp));if(!qp) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i],&n,qp);
        for(uint32_t j=0;j<n;j++) if(qp[j].queueCount && (qp[j].queueFlags&VK_QUEUE_COMPUTE_BIT)) { physical=devices[i];family=j;break; }
        free(qp);
        if(physical) fprintf(stderr,"Selected %s (capabilities only; runtime validation follows)\n",props.deviceName);
    }
    free(devices);if(!physical) return 0;
    VkPhysicalDeviceProperties2 props={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=asProps};
    vkGetPhysicalDeviceProperties2(physical,&props);
    float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceRayQueryFeaturesKHR rq={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,.rayQuery=VK_TRUE};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,.pNext=&rq,.accelerationStructure=VK_TRUE};
    VkPhysicalDeviceVulkan12Features v12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&as,.bufferDeviceAddress=VK_TRUE};
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&v12,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.enabledExtensionCount=3,.ppEnabledExtensionNames=extensions};
    if(!check(vkCreateDevice(physical,&dc,NULL,&device),"create ray query device")) return 0;
    vkGetDeviceQueue(device,family,0,&queue);vkGetPhysicalDeviceMemoryProperties(physical,&memory);
#define LOAD(name, target) target=(PFN_##name)vkGetDeviceProcAddr(device,#name); if(!target) return 0
    LOAD(vkCreateAccelerationStructureKHR,createAS);
    LOAD(vkDestroyAccelerationStructureKHR,destroyAS);
    LOAD(vkGetAccelerationStructureBuildSizesKHR,getSizes);
    LOAD(vkCmdBuildAccelerationStructuresKHR,buildAS);
    LOAD(vkGetAccelerationStructureDeviceAddressKHR,getASAddress);
#undef LOAD
    return 1;
}
static void barrier(VkCommandBuffer c,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkAccessFlags sa,VkAccessFlags da) {
    VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=sa,.dstAccessMask=da};
    vkCmdPipelineBarrier(c,src,dst,0,1,&b,0,NULL,0,NULL);
}
static int makeAS(VkAccelerationStructureTypeKHR type,VkAccelerationStructureGeometryKHR *geometry,uint32_t count,
                  VkAccelerationStructureKHR *handle,Buffer *storage,Buffer *scratch,VkAccelerationStructureBuildGeometryInfoKHR *build,uint32_t alignment) {
    *build=(VkAccelerationStructureBuildGeometryInfoKHR){.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,.type=type,.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,.geometryCount=1,.pGeometries=geometry};
    VkAccelerationStructureBuildSizesInfoKHR sizes={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getSizes(device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,build,&count,&sizes);
    if(!sizes.accelerationStructureSize || !sizes.buildScratchSize || !alignment) return 0;
    if(!buffer(storage,sizes.accelerationStructureSize,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,0) ||
       !buffer(scratch,sizes.buildScratchSize+alignment,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,0)) return 0;
    VkAccelerationStructureCreateInfoKHR ci={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,.buffer=storage->b,.size=sizes.accelerationStructureSize,.type=type};
    if(!check(createAS(device,&ci,NULL,handle),"create acceleration structure")) return 0;
    build->dstAccelerationStructure=*handle;
    build->scratchData.deviceAddress=(scratch->address+alignment-1)&~((VkDeviceAddress)alignment-1);
    return 1;
}
static void *readFile(const char *path,size_t max,size_t multiple,size_t *size) {
    FILE *f=fopen(path,"rb");if(!f) {perror(path);return NULL;}
    if(fseek(f,0,SEEK_END) || ftell(f)<=0) {fclose(f);return NULL;}
    long n=ftell(f);rewind(f);
    if((size_t)n>max || (size_t)n%multiple) {fprintf(stderr,"malformed/oversized file: %s\n",path);fclose(f);return NULL;}
    void *data=malloc((size_t)n);
    if(!data || fread(data,1,(size_t)n,f)!=(size_t)n) {free(data);fclose(f);return NULL;}
    fclose(f);*size=(size_t)n;return data;
}
static int uintArg(const char *s,uint32_t *value) {
    char *end;errno=0;unsigned long v=strtoul(s,&end,10);
    if(errno || !*s || *end || *s=='-' || v<1 || v>16384) return 0;
    *value=(uint32_t)v;return 1;
}
static int floatArg(const char *s,float *value) {
    char *end;errno=0;*value=strtof(s,&end);
    return !errno && *s && !*end && isfinite(*value) && *value>0 && *value<=1e6f;
}
static int writeOutputs(const char *prefix,const float *pixels,uint32_t w,uint32_t h) {
    size_t len=strlen(prefix);if(len>30000) return 0;
    char *path=malloc(len+32);if(!path) return 0;
    sprintf(path,"%s.depth.f32le",prefix);FILE *d=fopen(path,"wb");
    sprintf(path,"%s.normals.ppm",prefix);FILE *n=fopen(path,"wb");free(path);
    if(!d || !n) {if(d)fclose(d);if(n)fclose(n);return 0;}
    int ok=fprintf(n,"P6\n%u %u\n255\n",w,h)>0;
    for(size_t i=0;i<(size_t)w*h && ok;i++) {
        float z=pixels[i*4+3];uint32_t bits;memcpy(&bits,&z,4);
        unsigned char bytes[4]={(unsigned char)bits,(unsigned char)(bits>>8),(unsigned char)(bits>>16),(unsigned char)(bits>>24)};
        unsigned char rgb[3]={0,0,0};
        if(z>0) for(unsigned j=0;j<3;j++) {float v=fminf(1,fmaxf(0,pixels[i*4+j]*.5f+.5f));rgb[j]=(unsigned char)lroundf(v*255);}
        ok=fwrite(bytes,1,4,d)==4 && fwrite(rgb,1,3,n)==3;
    }
    if(fclose(d))ok=0;if(fclose(n))ok=0;return ok;
}
static int writeAO(const char *prefix,const float *pixels,uint32_t w,uint32_t h) {
    size_t len=strlen(prefix);if(len>30000) return 0;
    char *path=malloc(len+32);if(!path)return 0;
    sprintf(path,"%s.ao.f32le",prefix);FILE *f=fopen(path,"wb");
    sprintf(path,"%s.ao.ppm",prefix);FILE *p=fopen(path,"wb");free(path);
    if(!f || !p) {if(f)fclose(f);if(p)fclose(p);return 0;}
    int ok=fprintf(p,"P6\n%u %u\n255\n",w,h)>0;
    for(size_t i=0;i<(size_t)w*h && ok;i++) {
        float occ=pixels[i*4];
        if(!isfinite(occ) || occ<0 || occ>1) {ok=0;break;}
        uint32_t bits;memcpy(&bits,&occ,4);
        unsigned char bytes[4]={(unsigned char)bits,(unsigned char)(bits>>8),(unsigned char)(bits>>16),(unsigned char)(bits>>24)};
        unsigned char shade=pixels[i*4+3]>0?(unsigned char)lroundf((1-occ)*255):0;
        unsigned char rgb[]={shade,shade,shade};
        ok=fwrite(bytes,1,4,f)==4 && fwrite(rgb,1,3,p)==3;
    }
    if(fclose(f))ok=0;if(fclose(p))ok=0;return ok;
}
int main(int argc,char **argv) {
    int self=argc==3 && !strcmp(argv[1],"--self-test");
    int ao=argc==12 && !strcmp(argv[1],"--ao");
    uint32_t w=9,h=9;float sx=1,sy=1;
    uint32_t samples=0;float distance=0,bias=0;
    if(!self && !(((argc==9 && !strcmp(argv[1],"--scene")) || ao) && uintArg(argv[5],&w) && uintArg(argv[6],&h) && floatArg(argv[7],&sx) && floatArg(argv[8],&sy) &&
                 (!ao || (uintArg(argv[9],&samples) && samples<=256 && floatArg(argv[10],&distance) && floatArg(argv[11],&bias) && bias<distance && (uint64_t)w*h*samples<=268435456)))) {
        fprintf(stderr,"Usage: %s --self-test shader.spv\n       %s --scene triangles.f32le shader.spv output-prefix width height sx sy\n       %s --ao triangles.f32le shader.spv output-prefix width height sx sy samples distance bias\n"
                       "AO: samples 1..256, secondary ray budget <=268435456; finite 0<bias<distance<=1000000 scene units.\n"
                       "Deterministic Hammersley cosine hemisphere, camera-facing geometric normals.\n"
                       "Opaque double-sided captured geometry only; no materials/alpha or gameplay RT claim.\n"
                       "Outputs: .ao.f32le occlusion 0..1, .ao.ppm visibility white=open; primary misses float 0, black.\n",argv[0],argv[0],argv[0]);return 2;
    }
    uint32_t endian=1;if(*(unsigned char*)&endian!=1 || sizeof(float)!=4) {fprintf(stderr,"requires little-endian IEEE float32 host\n");return 2;}
    size_t triangleBytes=0,shaderBytes=0;
    float synthetic[]={-1,-1,3,1,-1,3,0,1,3};
    float *triangles=self?synthetic:readFile(argv[2],36000000,36,&triangleBytes);
    if(self)triangleBytes=sizeof(synthetic);
    if(!triangles)return 2;
    for(size_t i=0;i<triangleBytes/4;i++) if(!isfinite(triangles[i])) {fprintf(stderr,"nonfinite triangle coordinate\n");if(!self)free(triangles);return 2;}
    uint32_t *spirv=readFile(self?argv[2]:argv[3],16*1024*1024,4,&shaderBytes);
    if(!spirv || shaderBytes<20 || spirv[0]!=0x07230203) {fprintf(stderr,"invalid SPIR-V\n");free(spirv);if(!self)free(triangles);return 2;}
    int result=1;Buffer vertices={0},instances={0},out={0},bs={0},ts={0},bScratch={0},tScratch={0};
    VkAccelerationStructureKHR blas=0,tlas=0;VkShaderModule shader=0;VkDescriptorSetLayout layout=0;VkPipelineLayout pipelineLayout=0;
    VkPipeline pipeline=0;VkDescriptorPool descriptors=0;VkCommandPool pool=0;VkFence fence=0;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR ap={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    if(!initialize(&ap)) {fprintf(stderr,"UNAVAILABLE: Vulkan 1.2 ray query capabilities/device unavailable\n");result=77;goto done;}
    uint32_t triangleCount=(uint32_t)(triangleBytes/36);
    VkPhysicalDeviceProperties limits;vkGetPhysicalDeviceProperties(physical,&limits);
    size_t outputBytes=(size_t)w*h*16;
    if(triangleCount>ap.maxPrimitiveCount || !ap.maxInstanceCount || triangleBytes>limits.limits.maxStorageBufferRange || outputBytes>limits.limits.maxStorageBufferRange ||
       (w+7)/8>limits.limits.maxComputeWorkGroupCount[0] || (h+7)/8>limits.limits.maxComputeWorkGroupCount[1]) {fprintf(stderr,"UNAVAILABLE: scene exceeds device limits\n");result=77;goto done;}
    if(!buffer(&vertices,triangleBytes,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,1) ||
       !buffer(&instances,sizeof(VkAccelerationStructureInstanceKHR),VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,1) ||
       !buffer(&out,outputBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,1)) goto done;
    memcpy(vertices.map,triangles,triangleBytes);memset(out.map,0,outputBytes);
    VkAccelerationStructureGeometryKHR bg={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR,.flags=VK_GEOMETRY_OPAQUE_BIT_KHR};
    bg.geometry.triangles=(VkAccelerationStructureGeometryTrianglesDataKHR){.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT,.vertexData.deviceAddress=vertices.address,.vertexStride=12,.maxVertex=triangleCount*3-1,.indexType=VK_INDEX_TYPE_NONE_KHR};
    VkAccelerationStructureBuildGeometryInfoKHR bb,tb;
    if(!makeAS(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,&bg,triangleCount,&blas,&bs,&bScratch,&bb,ap.minAccelerationStructureScratchOffsetAlignment))goto done;
    VkAccelerationStructureDeviceAddressInfoKHR adi={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,.accelerationStructure=blas};
    VkDeviceAddress ba=getASAddress(device,&adi);if(!ba)goto done;
    VkAccelerationStructureInstanceKHR inst={.transform={.matrix={{1,0,0,0},{0,1,0,0},{0,0,1,0}}},.mask=255,.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,.accelerationStructureReference=ba};
    memcpy(instances.map,&inst,sizeof(inst));
    VkAccelerationStructureGeometryKHR tg={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR};
    tg.geometry.instances=(VkAccelerationStructureGeometryInstancesDataKHR){.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,.data.deviceAddress=instances.address};
    if(!makeAS(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,&tg,1,&tlas,&ts,&tScratch,&tb,ap.minAccelerationStructureScratchOffsetAlignment))goto done;
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=shaderBytes,.pCode=spirv};
    if(!check(vkCreateShaderModule(device,&sm,NULL,&shader),"create shader"))goto done;
    VkDescriptorSetLayoutBinding bindings[3]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},{.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},{.binding=2,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=3,.pBindings=bindings};
    if(!check(vkCreateDescriptorSetLayout(device,&lc,NULL,&layout),"descriptor layout"))goto done;
    VkPushConstantRange push={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=32};
    VkPipelineLayoutCreateInfo pc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    if(!check(vkCreatePipelineLayout(device,&pc,NULL,&pipelineLayout),"pipeline layout"))goto done;
    VkComputePipelineCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=pipelineLayout};
    if(!check(vkCreateComputePipelines(device,0,1,&cp,NULL,&pipeline),"compute pipeline"))goto done;
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};
    if(!check(vkCreateDescriptorPool(device,&dp,NULL,&descriptors),"descriptor pool"))goto done;
    VkDescriptorSet set;VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptors,.descriptorSetCount=1,.pSetLayouts=&layout};
    if(!check(vkAllocateDescriptorSets(device,&da,&set),"allocate descriptors"))goto done;
    VkWriteDescriptorSetAccelerationStructureKHR wa={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,.accelerationStructureCount=1,.pAccelerationStructures=&tlas};
    VkDescriptorBufferInfo vb={vertices.b,0,triangleBytes},ob={out.b,0,outputBytes};
    VkWriteDescriptorSet writes[]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.pNext=&wa,.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&vb},{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=2,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&ob}};
    vkUpdateDescriptorSets(device,3,writes,0,NULL);
    VkCommandPoolCreateInfo cpc={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    if(!check(vkCreateCommandPool(device,&cpc,NULL,&pool),"command pool"))goto done;
    VkCommandBuffer command;VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    if(!check(vkAllocateCommandBuffers(device,&ca,&command),"command buffer"))goto done;
    VkCommandBufferBeginInfo cb={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if(!check(vkBeginCommandBuffer(command,&cb),"begin commands"))goto done;
    barrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    VkAccelerationStructureBuildRangeInfoKHR br={.primitiveCount=triangleCount},tr={.primitiveCount=1};
    const VkAccelerationStructureBuildRangeInfoKHR *brp=&br,*trp=&tr;
    buildAS(command,1,&bb,&brp);
    barrier(command,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    buildAS(command,1,&tb,&trp);
    barrier(command,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,NULL);
    struct {uint32_t w,h;float sx,sy;uint32_t samples;float distance,bias;uint32_t ao;} parameters={w,h,sx,sy,samples,distance,bias,(uint32_t)ao};
    vkCmdPushConstants(command,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(parameters),&parameters);vkCmdDispatch(command,(w+7)/8,(h+7)/8,1);
    barrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
    if(!check(vkEndCommandBuffer(command),"end commands"))goto done;
    VkFenceCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};if(!check(vkCreateFence(device,&fc,NULL,&fence),"create fence"))goto done;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    if(!check(vkQueueSubmit(queue,1,&submit,fence),"submit geometry validation") || !check(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"wait geometry validation"))goto done;
    float *pixels=out.map;
    for(size_t i=0;i<(size_t)w*h*4;i++)if(!isfinite(pixels[i])) {fprintf(stderr,"nonfinite GPU result\n");goto done;}
    if(self) {
        size_t center=((size_t)(h/2)*w+w/2)*4;
        if(fabsf(pixels[center+3]-3)>1e-4f || fabsf(pixels[center])>1e-4f || fabsf(pixels[center+1])>1e-4f || fabsf(pixels[center+2]-1)>1e-4f || pixels[3]!=0 || pixels[((size_t)w*h-1)*4+3]!=0) {fprintf(stderr,"FAIL: center depth/normal or corner miss mismatch\n");goto done;}
        fprintf(stdout,"PASS: actual triangle center view-Z=3, geometric normal=(0,0,1), corner misses\n");result=0;
    } else if(ao) {
        if(writeAO(argv[4],pixels,w,h)) {
            fprintf(stdout,"Wrote %s.ao.f32le and %s.ao.ppm (%ux%u, %u triangles, %u deterministic cosine samples, distance=%g bias=%g scene units)\n",argv[4],argv[4],w,h,triangleCount,samples,(double)distance,(double)bias);
            fprintf(stdout,"Offline opaque double-sided captured geometry diagnostic; no material or gameplay RT validation.\n");result=0;
        } else fprintf(stderr,"AO output write failed\n");
    } else if(writeOutputs(argv[4],pixels,w,h)) {fprintf(stdout,"Wrote %s.depth.f32le and %s.normals.ppm (%ux%u, %u triangles)\n",argv[4],argv[4],w,h,triangleCount);result=0;}
    else fprintf(stderr,"output write failed\n");
done:
    if(device) {
        vkDeviceWaitIdle(device);
        if(fence)vkDestroyFence(device,fence,NULL);if(pool)vkDestroyCommandPool(device,pool,NULL);
        if(descriptors)vkDestroyDescriptorPool(device,descriptors,NULL);if(pipeline)vkDestroyPipeline(device,pipeline,NULL);
        if(pipelineLayout)vkDestroyPipelineLayout(device,pipelineLayout,NULL);if(layout)vkDestroyDescriptorSetLayout(device,layout,NULL);if(shader)vkDestroyShaderModule(device,shader,NULL);
        if(tlas)destroyAS(device,tlas,NULL);if(blas)destroyAS(device,blas,NULL);
        freeBuffer(&tScratch);freeBuffer(&bScratch);freeBuffer(&ts);freeBuffer(&bs);freeBuffer(&out);freeBuffer(&instances);freeBuffer(&vertices);
        vkDestroyDevice(device,NULL);
    }
    if(instance)vkDestroyInstance(instance,NULL);free(spirv);if(!self)free(triangles);return result;
}

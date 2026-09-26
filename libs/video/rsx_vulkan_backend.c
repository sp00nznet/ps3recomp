/*
 * ps3recomp - Vulkan RSX Backend, stage V0. See rsx_vulkan_backend.h.
 *
 * Deliberately synchronous: every clear and every present is recorded into a
 * one-shot command buffer, submitted, and waited on. That is slow and it is
 * also trivially correct, which is the property V0 needs. Batching per frame
 * comes with the draw path, once there is something worth batching.
 *
 * The colour target stays in VK_IMAGE_LAYOUT_GENERAL for its whole life:
 * legal for vkCmdClearColorImage, vkCmdCopyImageToBuffer and (later) as a
 * colour attachment, so no layout bookkeeping is needed yet.
 */

#ifndef _WIN32

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "rsx_vulkan_backend.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Run-time loaded entry points
 * -------------------------------------------------------------------------*/
static PFN_vkGetInstanceProcAddr                  pvkGetInstanceProcAddr;
static PFN_vkCreateInstance                       pvkCreateInstance;
static PFN_vkDestroyInstance                      pvkDestroyInstance;
static PFN_vkEnumeratePhysicalDevices             pvkEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties          pvkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties pvkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties    pvkGetPhysicalDeviceMemoryProperties;
static PFN_vkCreateDevice                         pvkCreateDevice;
static PFN_vkGetDeviceProcAddr                    pvkGetDeviceProcAddr;
static PFN_vkDestroyDevice                        pvkDestroyDevice;
static PFN_vkGetDeviceQueue                       pvkGetDeviceQueue;
static PFN_vkDeviceWaitIdle                       pvkDeviceWaitIdle;
static PFN_vkCreateImage                          pvkCreateImage;
static PFN_vkDestroyImage                         pvkDestroyImage;
static PFN_vkGetImageMemoryRequirements           pvkGetImageMemoryRequirements;
static PFN_vkBindImageMemory                      pvkBindImageMemory;
static PFN_vkCreateBuffer                         pvkCreateBuffer;
static PFN_vkDestroyBuffer                        pvkDestroyBuffer;
static PFN_vkGetBufferMemoryRequirements          pvkGetBufferMemoryRequirements;
static PFN_vkBindBufferMemory                     pvkBindBufferMemory;
static PFN_vkAllocateMemory                       pvkAllocateMemory;
static PFN_vkFreeMemory                           pvkFreeMemory;
static PFN_vkMapMemory                            pvkMapMemory;
static PFN_vkUnmapMemory                          pvkUnmapMemory;
static PFN_vkInvalidateMappedMemoryRanges         pvkInvalidateMappedMemoryRanges;
static PFN_vkCreateCommandPool                    pvkCreateCommandPool;
static PFN_vkDestroyCommandPool                   pvkDestroyCommandPool;
static PFN_vkAllocateCommandBuffers               pvkAllocateCommandBuffers;
static PFN_vkBeginCommandBuffer                   pvkBeginCommandBuffer;
static PFN_vkEndCommandBuffer                     pvkEndCommandBuffer;
static PFN_vkResetCommandBuffer                   pvkResetCommandBuffer;
static PFN_vkCmdPipelineBarrier                   pvkCmdPipelineBarrier;
static PFN_vkCmdClearColorImage                   pvkCmdClearColorImage;
static PFN_vkCmdCopyImageToBuffer                 pvkCmdCopyImageToBuffer;
static PFN_vkQueueSubmit                          pvkQueueSubmit;
static PFN_vkCreateFence                          pvkCreateFence;
static PFN_vkDestroyFence                         pvkDestroyFence;
static PFN_vkWaitForFences                        pvkWaitForFences;
static PFN_vkResetFences                          pvkResetFences;

/* ---------------------------------------------------------------------------
 * Backend state
 * -------------------------------------------------------------------------*/
typedef struct vk_state {
    void*            lib;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    u32              queue_family;
    VkPhysicalDeviceMemoryProperties memprops;

    u32              width, height;
    VkImage          color;
    VkDeviceMemory   color_mem;

    VkBuffer         readback;
    VkDeviceMemory   readback_mem;
    void*            readback_ptr;
    int              readback_coherent;

    VkCommandPool    pool;
    VkCommandBuffer  cmd;
    VkFence          fence;

    u32              clear_argb;      /* last NV4097 clear colour, ARGB8888 */
    u32              last_center;     /* 0x00RRGGBB of the last present      */
    u32              presented;       /* number of presents so far           */
    const char*      dump_path;
} vk_state;

static vk_state s_vk;

#define VK_LOG(...)  fprintf(stderr, "[RSX vulkan] " __VA_ARGS__)
#define VK_CHECK(call, what)                                               \
    do { VkResult r_ = (call);                                             \
         if (r_ != VK_SUCCESS) {                                           \
             VK_LOG("%s failed (VkResult %d)\n", (what), (int)r_);         \
             return -1; } } while (0)

/* ---------------------------------------------------------------------------
 * Loading
 * -------------------------------------------------------------------------*/
static int vk_load_library(void)
{
    static const char* names[] = { "libvulkan.so.1", "libvulkan.so" };
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !s_vk.lib; i++)
        s_vk.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!s_vk.lib) {
        VK_LOG("no Vulkan loader found (dlopen libvulkan.so.1: %s)\n", dlerror());
        return -1;
    }
    pvkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(s_vk.lib, "vkGetInstanceProcAddr");
    if (!pvkGetInstanceProcAddr) {
        VK_LOG("libvulkan has no vkGetInstanceProcAddr\n");
        return -1;
    }
    pvkCreateInstance = (PFN_vkCreateInstance)
        pvkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    return pvkCreateInstance ? 0 : -1;
}

#define LOAD_I(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(s_vk.instance, #name))) \
        { VK_LOG("missing instance function " #name "\n"); return -1; }
#define LOAD_D(name) \
    if (!(p##name = (PFN_##name)pvkGetDeviceProcAddr(s_vk.device, #name))) \
        { VK_LOG("missing device function " #name "\n"); return -1; }

static int vk_load_instance_functions(void)
{
    LOAD_I(vkDestroyInstance);
    LOAD_I(vkEnumeratePhysicalDevices);
    LOAD_I(vkGetPhysicalDeviceProperties);
    LOAD_I(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_I(vkGetPhysicalDeviceMemoryProperties);
    LOAD_I(vkCreateDevice);
    LOAD_I(vkGetDeviceProcAddr);
    return 0;
}

static int vk_load_device_functions(void)
{
    LOAD_D(vkDestroyDevice);            LOAD_D(vkGetDeviceQueue);
    LOAD_D(vkDeviceWaitIdle);
    LOAD_D(vkCreateImage);              LOAD_D(vkDestroyImage);
    LOAD_D(vkGetImageMemoryRequirements); LOAD_D(vkBindImageMemory);
    LOAD_D(vkCreateBuffer);             LOAD_D(vkDestroyBuffer);
    LOAD_D(vkGetBufferMemoryRequirements); LOAD_D(vkBindBufferMemory);
    LOAD_D(vkAllocateMemory);           LOAD_D(vkFreeMemory);
    LOAD_D(vkMapMemory);                LOAD_D(vkUnmapMemory);
    LOAD_D(vkInvalidateMappedMemoryRanges);
    LOAD_D(vkCreateCommandPool);        LOAD_D(vkDestroyCommandPool);
    LOAD_D(vkAllocateCommandBuffers);
    LOAD_D(vkBeginCommandBuffer);       LOAD_D(vkEndCommandBuffer);
    LOAD_D(vkResetCommandBuffer);
    LOAD_D(vkCmdPipelineBarrier);       LOAD_D(vkCmdClearColorImage);
    LOAD_D(vkCmdCopyImageToBuffer);
    LOAD_D(vkQueueSubmit);
    LOAD_D(vkCreateFence);              LOAD_D(vkDestroyFence);
    LOAD_D(vkWaitForFences);            LOAD_D(vkResetFences);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Device selection: PS3RECOMP_VK_DEVICE if set, else the first discrete GPU,
 * else the first integrated GPU, else anything that is not a CPU rasteriser,
 * else whatever is there (llvmpipe/lavapipe on a GPU-less CI runner).
 * -------------------------------------------------------------------------*/
static int vk_device_rank(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 0;
    default:                                     return 1;
    }
}

static int vk_find_graphics_queue(VkPhysicalDevice pd, u32* family)
{
    u32 n = 0;
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    if (n == 0 || n > 64) return -1;
    VkQueueFamilyProperties props[64];
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, props);
    for (u32 i = 0; i < n; i++)
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { *family = i; return 0; }
    return -1;
}

static int vk_pick_device(void)
{
    u32 n = 0;
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, NULL), "vkEnumeratePhysicalDevices");
    if (n == 0) { VK_LOG("no Vulkan physical devices\n"); return -1; }
    if (n > 16) n = 16;
    VkPhysicalDevice devs[16];
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, devs), "vkEnumeratePhysicalDevices");

    int best = -1, best_rank = -1;
    const char* forced = getenv("PS3RECOMP_VK_DEVICE");
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        pvkGetPhysicalDeviceProperties(devs[i], &p);
        u32 fam;
        int usable = vk_find_graphics_queue(devs[i], &fam) == 0;
        VK_LOG("device %u: %s (type %d, api %u.%u.%u)%s\n", i, p.deviceName,
               (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               usable ? "" : " -- no graphics queue");
        if (!usable) continue;
        if (forced) {
            if ((u32)atoi(forced) == i) { best = (int)i; break; }
            continue;
        }
        int rank = vk_device_rank(p.deviceType);
        if (rank > best_rank) { best = (int)i; best_rank = rank; }
    }
    if (best < 0) {
        if (forced) VK_LOG("PS3RECOMP_VK_DEVICE=%s is not a usable device\n", forced);
        else        VK_LOG("no device with a graphics queue\n");
        return -1;
    }
    s_vk.phys = devs[best];
    vk_find_graphics_queue(s_vk.phys, &s_vk.queue_family);
    pvkGetPhysicalDeviceMemoryProperties(s_vk.phys, &s_vk.memprops);
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    VK_LOG("using device %d: %s\n", best, p.deviceName);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Memory helpers
 * -------------------------------------------------------------------------*/
static int vk_find_memory_type(u32 type_bits, VkMemoryPropertyFlags want, u32* out)
{
    for (u32 i = 0; i < s_vk.memprops.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) &&
            (s_vk.memprops.memoryTypes[i].propertyFlags & want) == want) {
            *out = i; return 0;
        }
    return -1;
}

static int vk_create_color_target(void)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { s_vk.width, s_vk.height, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(pvkCreateImage(s_vk.device, &ici, NULL, &s_vk.color), "vkCreateImage");

    VkMemoryRequirements req;
    pvkGetImageMemoryRequirements(s_vk.device, s_vk.color, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) {
        VK_LOG("no memory type for the colour target\n"); return -1;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &s_vk.color_mem), "vkAllocateMemory(colour)");
    VK_CHECK(pvkBindImageMemory(s_vk.device, s_vk.color, s_vk.color_mem, 0), "vkBindImageMemory");
    return 0;
}

static int vk_create_readback(void)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize)s_vk.width * s_vk.height * 4u,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(pvkCreateBuffer(s_vk.device, &bci, NULL, &s_vk.readback), "vkCreateBuffer");

    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, s_vk.readback, &req);
    u32 type;
    const VkMemoryPropertyFlags vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coh = vis | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (vk_find_memory_type(req.memoryTypeBits, coh, &type) == 0) s_vk.readback_coherent = 1;
    else if (vk_find_memory_type(req.memoryTypeBits, vis, &type) == 0) s_vk.readback_coherent = 0;
    else { VK_LOG("no host-visible memory for readback\n"); return -1; }

    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &s_vk.readback_mem), "vkAllocateMemory(readback)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, s_vk.readback, s_vk.readback_mem, 0), "vkBindBufferMemory");
    VK_CHECK(pvkMapMemory(s_vk.device, s_vk.readback_mem, 0, VK_WHOLE_SIZE, 0, &s_vk.readback_ptr),
             "vkMapMemory");
    return 0;
}

/* ---------------------------------------------------------------------------
 * One-shot command submission
 * -------------------------------------------------------------------------*/
static const VkImageSubresourceRange k_color_range = {
    VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1
};

static int vk_begin(void)
{
    VK_CHECK(pvkResetCommandBuffer(s_vk.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(pvkBeginCommandBuffer(s_vk.cmd, &bi), "vkBeginCommandBuffer");
    return 0;
}

static int vk_submit_and_wait(void)
{
    VK_CHECK(pvkEndCommandBuffer(s_vk.cmd), "vkEndCommandBuffer");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd };
    VK_CHECK(pvkResetFences(s_vk.device, 1, &s_vk.fence), "vkResetFences");
    VK_CHECK(pvkQueueSubmit(s_vk.queue, 1, &si, s_vk.fence), "vkQueueSubmit");
    VK_CHECK(pvkWaitForFences(s_vk.device, 1, &s_vk.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    return 0;
}

/* Order this command buffer's work on the colour target after everything
 * submitted before it (the target never leaves GENERAL, so no transition). */
static void vk_barrier_color(VkImageLayout old_layout)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = old_layout,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s_vk.color,
        .subresourceRange = k_color_range,
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                          0, NULL, 0, NULL, 1, &b);
}

/* ---------------------------------------------------------------------------
 * rsx_backend callbacks
 * -------------------------------------------------------------------------*/
static void vk_cb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud; (void)depth; (void)stencil;   /* no depth/stencil target at V0 */
    s_vk.clear_argb = color;
    /* CLEAR_SURFACE: 0xF0 are the four colour channels (same test as the null
     * backend -- per-channel masking comes with the colour-mask state). */
    if (!(flags & 0xF0u) || !s_vk.device) return;

    VkClearColorValue v;
    v.float32[0] = (float)((color >> 16) & 0xFFu) / 255.0f;   /* R */
    v.float32[1] = (float)((color >>  8) & 0xFFu) / 255.0f;   /* G */
    v.float32[2] = (float)( color        & 0xFFu) / 255.0f;   /* B */
    v.float32[3] = (float)((color >> 24) & 0xFFu) / 255.0f;   /* A */

    if (vk_begin()) return;
    vk_barrier_color(VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearColorImage(s_vk.cmd, s_vk.color, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &k_color_range);
    vk_submit_and_wait();
}

static void vk_dump_ppm(const unsigned char* rgba)
{
    FILE* f = fopen(s_vk.dump_path, "wb");
    if (!f) { VK_LOG("cannot open %s for the frame dump\n", s_vk.dump_path); return; }
    fprintf(f, "P6\n%u %u\n255\n", s_vk.width, s_vk.height);
    for (size_t i = 0; i < (size_t)s_vk.width * s_vk.height; i++)
        fwrite(rgba + i * 4, 1, 3, f);
    fclose(f);
}

static void vk_cb_present(void* ud, u32 buffer_id)
{
    (void)ud; (void)buffer_id;
    if (!s_vk.device) return;

    if (vk_begin()) return;
    vk_barrier_color(VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = {
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { s_vk.width, s_vk.height, 1 },
    };
    pvkCmdCopyImageToBuffer(s_vk.cmd, s_vk.color, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.readback, 1, &region);
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_vk.readback, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &host, 0, NULL);
    if (vk_submit_and_wait()) return;

    if (!s_vk.readback_coherent) {
        VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                  .memory = s_vk.readback_mem, .offset = 0, .size = VK_WHOLE_SIZE };
        pvkInvalidateMappedMemoryRanges(s_vk.device, 1, &r);
    }
    const unsigned char* px = (const unsigned char*)s_vk.readback_ptr;
    size_t c = ((size_t)(s_vk.height / 2) * s_vk.width + s_vk.width / 2) * 4;
    s_vk.last_center = ((u32)px[c] << 16) | ((u32)px[c + 1] << 8) | (u32)px[c + 2];
    s_vk.presented++;
    if (s_vk.dump_path) vk_dump_ppm(px);
}

static rsx_backend s_vulkan_backend = {
    .userdata = &s_vk,
    .clear    = vk_cb_clear,
    .present  = vk_cb_present,
};

/* ---------------------------------------------------------------------------
 * Public entry points
 * -------------------------------------------------------------------------*/
static int vk_init_all(u32 width, u32 height)
{
    s_vk.width  = width  ? width  : 1280;
    s_vk.height = height ? height : 720;
    s_vk.dump_path = getenv("PS3RECOMP_VK_DUMP");

    if (vk_load_library()) return -1;

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "ps3recomp",
                              .pEngineName = "ps3recomp RSX",
                              .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app };
    VK_CHECK(pvkCreateInstance(&ci, NULL, &s_vk.instance), "vkCreateInstance");
    if (vk_load_instance_functions()) return -1;
    if (vk_pick_device()) return -1;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = s_vk.queue_family,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VK_CHECK(pvkCreateDevice(s_vk.phys, &dci, NULL, &s_vk.device), "vkCreateDevice");
    if (vk_load_device_functions()) return -1;
    pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 0, &s_vk.queue);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = s_vk.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, &s_vk.cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.fence), "vkCreateFence");

    if (vk_create_color_target()) return -1;
    if (vk_create_readback()) return -1;

    /* UNDEFINED -> GENERAL once, then clear to opaque black so a present
     * before the guest's first clear reads defined memory. */
    if (vk_begin()) return -1;
    vk_barrier_color(VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    pvkCmdClearColorImage(s_vk.cmd, s_vk.color, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &k_color_range);
    if (vk_submit_and_wait()) return -1;
    return 0;
}

int rsx_vulkan_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_vk, 0, sizeof s_vk);
    if (vk_init_all(width, height)) {
        rsx_vulkan_backend_shutdown();
        return -1;
    }
    rsx_set_backend(&s_vulkan_backend);
    VK_LOG("offscreen target %ux%u ready (%s)\n", s_vk.width, s_vk.height, title ? title : "");
    return 0;
}

void rsx_vulkan_backend_shutdown(void)
{
    if (rsx_get_backend() == &s_vulkan_backend) rsx_set_backend(NULL);
    if (s_vk.device) {
        pvkDeviceWaitIdle(s_vk.device);
        if (s_vk.readback_ptr) pvkUnmapMemory(s_vk.device, s_vk.readback_mem);
        if (s_vk.readback)     pvkDestroyBuffer(s_vk.device, s_vk.readback, NULL);
        if (s_vk.readback_mem) pvkFreeMemory(s_vk.device, s_vk.readback_mem, NULL);
        if (s_vk.color)        pvkDestroyImage(s_vk.device, s_vk.color, NULL);
        if (s_vk.color_mem)    pvkFreeMemory(s_vk.device, s_vk.color_mem, NULL);
        if (s_vk.fence)        pvkDestroyFence(s_vk.device, s_vk.fence, NULL);
        if (s_vk.pool)         pvkDestroyCommandPool(s_vk.device, s_vk.pool, NULL);
        pvkDestroyDevice(s_vk.device, NULL);
    }
    if (s_vk.instance && pvkDestroyInstance) pvkDestroyInstance(s_vk.instance, NULL);
    if (s_vk.lib) dlclose(s_vk.lib);
    memset(&s_vk, 0, sizeof s_vk);
}

int rsx_vulkan_backend_pump_messages(void) { return 0; }

void rsx_vulkan_backend_present(void) { vk_cb_present(&s_vk, 0); }

u32 rsx_vulkan_backend_debug_color(void) { return s_vk.clear_argb; }

u32 rsx_vulkan_backend_readback_center(void)
{
    return s_vk.presented ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_guest_draws(void) { return 0; }

#endif /* !_WIN32 */

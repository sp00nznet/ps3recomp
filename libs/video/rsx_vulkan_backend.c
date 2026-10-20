/*
 * ps3recomp - Vulkan RSX Backend. See rsx_vulkan_backend.h.
 *
 * The file, in order:
 *   loader and device      libvulkan by dlopen, Vulkan 1.0 entry points only;
 *                          device choice, BC textures when the device has them
 *   memory, images, submit helpers
 *   vtable fallback path   the null backend's fixed-function contract:
 *                          position = attribute 0, flat colour = attribute 3
 *                          of each triangle's first vertex, texcoord0 =
 *                          attribute 8, fetched and expanded through the
 *                          shared rsx_fetch_attrib(); unit 0 decoded through
 *                          rsx_texture_decode(); depth test, no blend/stencil
 *   vtable guest programs  the same path running the decompilers' HLSL, with
 *                          whole mip chains and per-unit samplers
 *   optional window        SDL2 + swapchain; a present blits the frame into it
 *   draw engine backend    rsx_draw_backend: targets, clears, present and
 *                          readback (E1), pipelines and draws (E2), textures,
 *                          surface views and samplers (E3), depth snapshots
 *                          (E4) -- the stage names in the commit history
 *   public entry points    init picks the path, shutdown undoes everything
 *
 * Deliberately synchronous: every clear, draw and present is one one-shot
 * command buffer, submitted and waited on. Slow and trivially correct; it is
 * what lets the engine's submit_and_wait and staging reuse cost nothing, and
 * batching is the first thing to change once correctness is settled.
 *
 * Images stay in VK_IMAGE_LAYOUT_GENERAL for their whole life: legal for
 * transfers, sampling and as attachments, so there is no layout bookkeeping.
 *
 * Shaders: libs/video/vulkan/rsx_vk_fallback.{vert,frag}, embedded as SPIR-V.
 * Regenerate after editing them with:
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_vert_spv \
 *       -o rsx_vk_fallback_vert.spv.h rsx_vk_fallback.vert
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_frag_spv \
 *       -o rsx_vk_fallback_frag.spv.h rsx_vk_fallback.frag
 * then `expand -t 4` both outputs: glslang indents with tabs, the project
 * with spaces.
 */


#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "rsx_vulkan_backend.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"
#include "rsx_shader_spirv.h"
#include "rsx_draw_engine.h"
#include "rsx_resolution_scale.h"
#include "rsx_display_geometry.h"
#include "rsx_vulkan_anisotropy.h"
#include "rsx_folklore_composite.h"
#include "rsx_depth_clip.h"
#include "rsx_frame_dump.h"
#ifdef PS3RECOMP_RSX_CACAO
#define FFX_CACAO_ENABLE_VULKAN
#include "ffx_cacao_impl.h"
#include "rsx_cacao_dispatch.h"
#endif
#include "ps3emu/strict_boot.h"
#include "vulkan/rsx_vk_fallback_vert.spv.h"
#include "vulkan/rsx_vk_fallback_frag.spv.h"

/* SDL2 is already required off Windows (cellPad, cellAudio); here it only
 * opens the optional window. Same include style as those two. */
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include "../input/pad_keyboard_sdl_events.h"

#include "rsx_vulkan_loader.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Run-time loaded entry points (Vulkan 1.0 core only)
 * -------------------------------------------------------------------------*/
#define VK_GLOBAL_FNS(X) X(vkCreateInstance)
#define VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FNS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkQueueSubmit) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) \
    X(vkCmdSetStencilReference) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdBlitImage)
/* Window-system entry points, loaded only when a window was requested, so a
 * headless run never depends on a driver that can present. */
#define VK_WSI_INSTANCE_FNS(X) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)
#define VK_WSI_DEVICE_FNS(X) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

#define VK_DECLARE(name) static PFN_##name p##name;
static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr;
VK_GLOBAL_FNS(VK_DECLARE)
VK_INSTANCE_FNS(VK_DECLARE)
VK_DEVICE_FNS(VK_DECLARE)
VK_WSI_INSTANCE_FNS(VK_DECLARE)
VK_WSI_DEVICE_FNS(VK_DECLARE)

/* ---------------------------------------------------------------------------
 * Backend state
 * -------------------------------------------------------------------------*/
typedef struct vk_vertex {
    float pos[4];
    float col[4];
    float uv[2];
} vk_vertex;

typedef struct vk_buffer {
    VkBuffer       buf;
    VkDeviceMemory mem;
    void*          ptr;
    VkDeviceSize   size;
    int            coherent;
} vk_buffer;

typedef struct vk_image {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
} vk_image;

#define VK_SAMPLER_CACHE 32
#define VK_PIPELINE_SLOTS (2 * 2 * 8)   /* ztest x zwrite x compare op */

typedef struct vk_state {
    rsx_vk_library   lib;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    u32              queue_family;
    VkPhysicalDeviceMemoryProperties memprops;

    u32              width, height;
    rsx_display_rect content_rect;  /* valid image inside the output canvas */
    float present_aspect;
    int              fxaa_enabled;
    u64              fxaa_frames;
    int              fsr1_enabled, fsr1_failed, fsr1_scale_fallback;
    u64              fsr1_frames;
    int              fsr1_sharpened, rcas_failed;   /* this frame's FSR1 ran RCAS; RCAS pass broken */
    u64              rcas_frames;
    VkImage          display_image;
    vk_image         color;           /* R8G8B8A8_UNORM                 */
    vk_image         depth;           /* D32_SFLOAT or fallback         */
    VkFormat         depth_format;
    VkImageAspectFlags depth_aspect;  /* DEPTH, plus STENCIL if it has one  */

    VkRenderPass     render_pass;
    VkFramebuffer    framebuffer;
    VkShaderModule   vs, fs;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet  desc_set;
    VkPipelineLayout pipe_layout;
    VkPipeline       pipelines[VK_PIPELINE_SLOTS];
    VkSampler        sampler;

    vk_image         dummy;           /* 1x1 magenta: "nothing bound"    */
    vk_image         tex[RSX_MAX_TEXTURES];   /* decoded guest textures  */
    int              tex_ready[RSX_MAX_TEXTURES];
    /* Guest path only: each unit's sampler, built from its wrap, filter and
     * LOD registers (the fallback path keeps `sampler`, the null backend's
     * nearest + repeat). Cached by register value; VK_NULL_HANDLE = use
     * `sampler`. */
    VkSampler        tex_sampler[RSX_MAX_TEXTURES];
    struct { u64 key; VkSampler s; } samp_cache[VK_SAMPLER_CACHE];
    u32              samp_count;

    vk_buffer        vertices;        /* expanded triangle list          */
    vk_vertex*       cpu_verts;
    size_t           cpu_cap, cpu_count;

    vk_buffer        readback;
    VkCommandPool    pool;
    VkCommandBuffer  cmd;
    VkFence          fence;

    /* Optional window (PS3RECOMP_VK_WINDOW=1). Rendering stays offscreen
     * either way -- readback and the tests are unchanged -- and a present
     * additionally blits the frame into the window's swapchain. */
    int              eng_active;      /* the register-file engine drives us */
    int              bc_ok;           /* textureCompressionBC enabled       */
    int              alpha_to_one_ok; /* alphaToOne enabled when supported */
    int              anisotropy_ok;
    int              rt_requested, rt_enabled; /* explicit rayquery mode only */
#ifdef PS3RECOMP_RSX_CACAO
    int              cacao_api_ok, cacao_device_ok;
#endif
    float            max_anisotropy;
    u32              anisotropy_override; /* Immutable for this device lifetime. */
    u32              max_dim;         /* maxImageDimension2D of the device  */
    int              windowed;
    int              selected_display; /* SDL display index for window/fullscreen */
    SDL_Window*      window;
    const char*      inst_exts[16];
    u32              inst_ext_count;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkExtent2D       sc_extent;
    u32              sc_count;
    VkImage          sc_images[8];
    VkSemaphore      sc_done[8];      /* per image: the present waits on it */
    VkSemaphore      sem_acquire;
    double           hold_seconds;

    const rsx_state* state;           /* live RSX state, for draws       */
    u32              clear_argb;
    u32              last_center;
    u32              presented;
    u64              successful_window_presents;
    u64              successful_offscreen_frames; /* completed GPU readbacks */
    int              swapchain_dirty;
    int              present_deferred;
    const char*      dump_path;
    u32              dump_every;       /* write every nth presented frame */
} vk_state;

static vk_state s_vk;

#define VK_LOG(...)  fprintf(stderr, "[RSX vulkan] " __VA_ARGS__)

#include "rsx_vulkan_pipeline_cache.inc"
#include "rsx_vulkan_pipeline_timing.inc"
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
    char error[512];
    s_vk.lib = rsx_vk_library_open(error, sizeof error);
    if (!s_vk.lib) {
        VK_LOG("%s\n", error);
        return -1;
    }
    pvkGetInstanceProcAddr = rsx_vk_library_get_proc(s_vk.lib, error, sizeof error);
    if (!pvkGetInstanceProcAddr) { VK_LOG("%s\n", error); return -1; }
#define LOAD_G(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(VK_NULL_HANDLE, #name))) \
        { VK_LOG("missing global function " #name "\n"); return -1; }
    VK_GLOBAL_FNS(LOAD_G)
    return 0;
}

static int vk_load_instance_functions(void)
{
#define LOAD_I(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(s_vk.instance, #name))) \
        { VK_LOG("missing instance function " #name "\n"); return -1; }
    VK_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_wsi_instance_functions(void)
{
    VK_WSI_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_device_functions(void)
{
#define LOAD_D(name) \
    if (!(p##name = (PFN_##name)pvkGetDeviceProcAddr(s_vk.device, #name))) \
        { VK_LOG("missing device function " #name "\n"); return -1; }
    VK_DEVICE_FNS(LOAD_D)
    return 0;
}

static int vk_load_wsi_device_functions(void)
{
    VK_WSI_DEVICE_FNS(LOAD_D)
    return 0;
}

/* ---------------------------------------------------------------------------
 * Device selection: PS3RECOMP_VK_DEVICE if set, else the first discrete GPU,
 * else integrated, else anything that is not a CPU rasteriser, else whatever
 * is there (llvmpipe/lavapipe on a GPU-less CI runner).
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
    for (u32 i = 0; i < n; i++) {
        if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (s_vk.windowed) {           /* the same queue must also present */
            VkBool32 ok = VK_FALSE;
            if (pvkGetPhysicalDeviceSurfaceSupportKHR(pd, i, s_vk.surface, &ok) != VK_SUCCESS || !ok)
                continue;
        }
        *family = i; return 0;
    }
    return -1;
}

static int vk_has_device_extension(VkPhysicalDevice pd, const char* name)
{
    u32 n = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkExtensionProperties* e = (VkExtensionProperties*)malloc(n * sizeof *e);
    if (!e) return 0;
    int found = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) == VK_SUCCESS)
        for (u32 i = 0; i < n && !found; i++) found = strcmp(e[i].extensionName, name) == 0;
    free(e);
    return found;
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
        int usable = vk_find_graphics_queue(devs[i], &fam) == 0 &&
                     (!s_vk.windowed ||
                      vk_has_device_extension(devs[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        VK_LOG("device %u: %s (type %d, api %u.%u.%u)%s\n", i, p.deviceName,
               (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               usable ? "" : (s_vk.windowed ? " -- cannot present to the window"
                                            : " -- no graphics queue"));
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
    /* Vulkan guarantees 4096; the engine's targets and textures may use what
     * the device really has (a title's 4608-wide target on RADV). */
    s_vk.max_dim = p.limits.maxImageDimension2D;
    s_vk.max_anisotropy = p.limits.maxSamplerAnisotropy;
    VK_LOG("using device %d: %s\n", best, p.deviceName);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Memory, images, buffers
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

static int vk_create_image_levels_samples(vk_image* im, VkFormat fmt, u32 w, u32 h, u32 levels,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect, u32 samples)
{
    if (!levels) levels = 1;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { w, h, 1 }, .mipLevels = levels, .arrayLayers = 1,
        .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(pvkCreateImage(s_vk.device, &ici, NULL, &im->img), "vkCreateImage");
    VkMemoryRequirements req;
    pvkGetImageMemoryRequirements(s_vk.device, im->img, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) {
        VK_LOG("no memory type for an image\n"); return -1;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &im->mem), "vkAllocateMemory(image)");
    VK_CHECK(pvkBindImageMemory(s_vk.device, im->img, im->mem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { aspect, 0, levels, 0, 1 },
    };
    VK_CHECK(pvkCreateImageView(s_vk.device, &vci, NULL, &im->view), "vkCreateImageView");
    return 0;
}

static int vk_create_image_levels(vk_image* im, VkFormat fmt, u32 w, u32 h, u32 levels,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    return vk_create_image_levels_samples(im, fmt, w, h, levels, usage, aspect, 1);
}

static int vk_create_image(vk_image* im, VkFormat fmt, u32 w, u32 h,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    return vk_create_image_levels(im, fmt, w, h, 1, usage, aspect);
}

static void vk_flush_pending_impl(int reason, const char* site);
#define vk_flush_pending(reason) vk_flush_pending_impl((reason), __func__)

static void vk_destroy_image(vk_image* im)
{
    vk_flush_pending(1);
    if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
    if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
    if (im->mem)  pvkFreeMemory(s_vk.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static int vk_create_host_buffer(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    const VkMemoryPropertyFlags vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coh = vis | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    /* A buffer the GPU only copies into is one the CPU reads back, and the
     * first coherent type is usually write-combined and uncached (RADV's
     * GTT): there the readback memcpy was 82% of the FIFO thread's time in
     * Twisted Metal. Take a cached type for it when there is one; reads
     * invalidate, so a non-coherent one is fine. */
    const VkMemoryPropertyFlags cached = vis | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const int readback = usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    /* Opt-in experiment: CPU-cached coherent uploads avoid slow writes into
     * device-local BAR mappings on discrete GPUs. Keep the existing choice
     * by default until matched-scene tests establish the tradeoff. */
    const char* cached_upload = getenv("PS3RECOMP_VK_CACHED_UPLOAD");
    const int prefer_cached = !readback && cached_upload && !strcmp(cached_upload, "1");
    if (readback && vk_find_memory_type(req.memoryTypeBits, cached | coh, &type) == 0) b->coherent = 1;
    else if (readback && vk_find_memory_type(req.memoryTypeBits, cached, &type) == 0) b->coherent = 0;
    else if (prefer_cached && vk_find_memory_type(req.memoryTypeBits, cached | coh, &type) == 0) b->coherent = 1;
    else if (vk_find_memory_type(req.memoryTypeBits, coh, &type) == 0) b->coherent = 1;
    else if (vk_find_memory_type(req.memoryTypeBits, vis, &type) == 0) b->coherent = 0;
    else { VK_LOG("no host-visible memory for a buffer\n"); return -1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &b->mem), "vkAllocateMemory(buffer)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    VK_CHECK(pvkMapMemory(s_vk.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->ptr), "vkMapMemory");
    b->size = size;
    if (prefer_cached) VK_LOG("cached upload: usage=0x%X bytes=%llu type=%u flags=0x%X\n",
        (unsigned)usage, (unsigned long long)size, type,
        (unsigned)s_vk.memprops.memoryTypes[type].propertyFlags);
    return 0;
}

static void vk_destroy_buffer(vk_buffer* b)
{
    vk_flush_pending(1);
    if (b->ptr) pvkUnmapMemory(s_vk.device, b->mem);
    if (b->buf) pvkDestroyBuffer(s_vk.device, b->buf, NULL);
    if (b->mem) pvkFreeMemory(s_vk.device, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* Host writes to non-coherent memory need a flush; there is no
 * vkFlushMappedMemoryRanges loaded, so writes only ever go to coherent
 * buffers (every desktop/L4T driver exposes one). Reads invalidate. */
static void vk_invalidate(vk_buffer* b)
{
    if (b->coherent) return;
    VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                              .memory = b->mem, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkInvalidateMappedMemoryRanges(s_vk.device, 1, &r);
}

/* ---------------------------------------------------------------------------
 * One-shot command submission, with engine draw batching
 *
 * Every operation used to be begun, submitted and waited on alone, so the
 * renderer thread paid a full GPU round trip per guest draw (about 30% of
 * its time in Folklore gameplay on an RTX 5080). Engine draws now stay
 * recorded in s_vk.cmd and unsubmitted (s_batch.pending); their vertex,
 * index and constant data and descriptor sets are ring-allocated per draw so
 * nothing a pending draw reads is rewritten. Batches exist only between
 * draws: every other backend entry point, any object destruction and any
 * ring/pool exhaustion flushes first (vk_flush_pending), so all non-draw
 * paths still start from an idle GPU exactly as before. A full memory
 * barrier precedes each batched draw, keeping the old serial order and
 * visibility. PS3RECOMP_VK_BATCH=0 restores one wait per draw;
 * PS3RECOMP_VK_BATCH_STATS=1 logs draws per submission and flush reasons.
 * -------------------------------------------------------------------------*/
#define VK_BATCH_SETS      1024u              /* draws (descriptor sets) per batch */
#define VK_BATCH_VB_MIN    (8u << 20)         /* vertex ring floor */
#define VK_BATCH_IB_MIN    (2u << 20)         /* index ring floor */
#define VK_BATCH_UBO_BYTES (16u << 20)        /* constants ring */
/* Texture uploads are deferred the same way: their bytes go into a staging
 * ring and their copies are recorded after the batched draws (each behind a
 * full barrier, as before) instead of one allocate/submit/wait/free each.
 * A save load in Folklore issued 724 of those in one frame (~120 ms of
 * waits plus allocation). PS3RECOMP_VK_DEFER_UPLOADS=0 restores them. */
#define VK_UPLOAD_RING_MIN (32u << 20)
#define VK_UPLOAD_RING_MAX (512u << 20)
#define VK_UPLOAD_ALIGN    256u
static struct {
    int on, configured, stats, failed;
    int defer;                     /* texture uploads deferred into the batch */
    u32 deferred;                  /* deferred non-draw commands recorded in s_vk.cmd */
    vk_buffer ring;                /* upload staging ring */
    VkDeviceSize rc;               /* its cursor */
    u64 deferred_uploads;
    int merge;                     /* consecutive batched draws share a render pass */
    u64 merged;
    u32 max;                       /* draws per batch (PS3RECOMP_VK_BATCH_MAX) */
    u32 trace;                     /* PS3RECOMP_VK_BATCH_TRACE: events left to log */
    int recording;                 /* s_vk.cmd begun, not yet submitted */
    u32 pending;                   /* engine draws recorded in it */
    u32 sets;                      /* descriptor sets taken from pool */
    VkDeviceSize vb, ib, ub;       /* ring cursors */
    VkDeviceSize ualign;
    vk_buffer ubo;
    VkDescriptorPool pool;
    u64 submits, draws, batched, flush[4], last_report;
} s_batch;

static void vk_batch_configure(void)
{
    if (s_batch.configured) return;
    s_batch.configured = 1;
    const char* e = getenv("PS3RECOMP_VK_BATCH");
    s_batch.on = !(e && e[0] == '0');
    const char* st = getenv("PS3RECOMP_VK_BATCH_STATS");
    s_batch.stats = st && st[0] && st[0] != '0';
    const char* tr = getenv("PS3RECOMP_VK_BATCH_TRACE");
    s_batch.trace = tr ? (u32)strtoul(tr, NULL, 10) : 0;
    const char* mx = getenv("PS3RECOMP_VK_BATCH_MAX");
    s_batch.max = mx ? (u32)strtoul(mx, NULL, 10) : VK_BATCH_SETS;
    if (!s_batch.max || s_batch.max > VK_BATCH_SETS) s_batch.max = VK_BATCH_SETS;
    const char* du = getenv("PS3RECOMP_VK_DEFER_UPLOADS");
    s_batch.defer = s_batch.on && !(du && du[0] == '0');
    const char* mp = getenv("PS3RECOMP_VK_MERGE_PASSES");
    s_batch.merge = s_batch.on && !(mp && mp[0] == '0');
    VK_LOG("engine draw batching: %s, texture uploads %s, render passes %s\n",
           s_batch.on ? "on" : "off (PS3RECOMP_VK_BATCH=0)",
           s_batch.defer ? "deferred" : "waited", s_batch.merge ? "merged" : "per draw");
}

/* Diagnostic: the backend assumes one calling thread. */
static void vk_batch_thread_check(const char* where)
{
    static unsigned long owner, reports;
    const unsigned long me = (unsigned long)SDL_ThreadID();
    if (!owner) owner = me;
    else if (me != owner && reports < 40) {
        reports++;
        VK_LOG("batch-thread: %s on thread %lu, first user %lu (pending=%u)\n", where, me, owner, s_batch.pending);
    }
}

/* The render pass batched draws leave open for the next draw into the same
 * targets (vk_eng_draw). Anything else recorded or submitted closes it. */
static struct {
    int open;
    VkRenderPass rp;
    VkFramebuffer fb;
    u32 w, h;
} s_rp;

/* Released textures that recorded, unsubmitted commands may still sample
 * (vk_eng_release); destroyed after the submission that ends them. */
#define VK_GRAVE_MAX 256u
static struct { vk_image im[VK_GRAVE_MAX]; u32 n; } s_grave;

/* Asynchronous ray transport (rsx_vulkan_ray_ao.inc): a second queue of the
 * graphics family traces one frame while the next frame renders. Its
 * submissions are not covered by s_vk.fence, so anything that destroys an
 * image the trace may still sample drains it first. */
static struct {
    VkQueue queue; VkCommandBuffer cmd; VkFence fence; int available, inflight;
    /* ready: the transport's main submission -> the async trace (its inputs).
     * done: the async trace -> the next transport main submission (history).
     * attach: the next vk_submit_and_wait carries them (transport only). */
    VkSemaphore ready, done; int attach, done_pending;
    /* recorded: a trace waiting for the frame's present to be submitted
     * (recorded_ok: its command buffer is valid; otherwise the submission only
     * consumes the ready signal). */
    int recorded, recorded_ok;
    u64 kicked;
} s_async;
/* Submit the recorded trace: right after the frame's present, it overlaps the
 * next frame's command processing instead of delaying this frame's present. */
static void vk_async_kick(void)
{
    if (!s_async.recorded) return;
    s_async.recorded = 0;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
        .pWaitSemaphores = &s_async.ready, .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = s_async.recorded_ok ? 1u : 0u, .pCommandBuffers = &s_async.cmd,
        .signalSemaphoreCount = s_async.recorded_ok ? 1u : 0u, .pSignalSemaphores = &s_async.done };
    if (pvkResetFences(s_vk.device, 1, &s_async.fence) != VK_SUCCESS ||
        pvkQueueSubmit(s_async.queue, 1, &si, s_async.fence) != VK_SUCCESS) {
        VK_LOG("asynchronous ray transport submission failed; transport stays synchronous\n");
        pvkDeviceWaitIdle(s_vk.device);
        s_async.available = 0; s_async.inflight = 0; s_async.done_pending = 0;
        return;
    }
    s_async.inflight = 1; s_async.done_pending = s_async.recorded_ok; s_async.kicked++;
}
static void vk_async_drain(void)
{
    vk_async_kick();
    if (!s_async.inflight) return;
    pvkWaitForFences(s_vk.device, 1, &s_async.fence, VK_TRUE, UINT64_MAX);
    s_async.inflight = 0;
}

static void vk_grave_empty(void)
{
    if (s_grave.n) vk_async_drain();
    for (u32 i = 0; i < s_grave.n; i++) {
        vk_image* im = &s_grave.im[i];
        if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
        if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
        if (im->mem)  pvkFreeMemory(s_vk.device, im->mem, NULL);
    }
    s_grave.n = 0;
}

static void vk_rp_close(void)
{
    if (!s_rp.open) return;
    pvkCmdEndRenderPass(s_vk.cmd);
    s_rp.open = 0;
}

static int vk_begin(void)
{
    vk_batch_thread_check("begin");
    /* Append after batched draws and uploads; otherwise start fresh, as before. */
    if (s_batch.recording && (s_batch.pending || s_batch.deferred)) { vk_rp_close(); return 0; }
    s_rp.open = 0;
    s_batch.recording = 0;
    VK_CHECK(pvkResetCommandBuffer(s_vk.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(pvkBeginCommandBuffer(s_vk.cmd, &bi), "vkBeginCommandBuffer");
    s_batch.recording = 1;
    return 0;
}

#include "rsx_vk_host_profile.h"

static int vk_submit_and_wait_impl(const char* site)
{
    vk_batch_thread_check("submit");
    vk_rp_close();
    const u32 drawn = s_batch.pending;
    if (s_batch.trace) { s_batch.trace--; VK_LOG("batch-trace: submit pending=%u recording=%d\n", drawn, s_batch.recording); }
    s_batch.recording = 0;
    s_batch.pending = 0;
    s_batch.deferred = 0;
    const Uint64 hp_begin = vhp_tick();
    Uint64 hp_queued = hp_begin;
    VkResult r = pvkEndCommandBuffer(s_vk.cmd);
    if (r == VK_SUCCESS) {
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd };
        const VkPipelineStageFlags async_wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if (s_async.attach) {
            s_async.attach = 0;
            if (s_async.done_pending) {
                si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s_async.done;
                si.pWaitDstStageMask = &async_wait; s_async.done_pending = 0;
            }
            si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s_async.ready;
        }
        r = pvkResetFences(s_vk.device, 1, &s_vk.fence);
        if (r == VK_SUCCESS) r = pvkQueueSubmit(s_vk.queue, 1, &si, s_vk.fence);
        hp_queued = vhp_tick();
        if (r == VK_SUCCESS) r = pvkWaitForFences(s_vk.device, 1, &s_vk.fence, VK_TRUE, UINT64_MAX);
    }
    const Uint64 hp_waited = vhp_tick();
    if (r == VK_SUCCESS) vk_grave_empty();
    /* Whether it ran or failed, nothing recorded is pending any more. */
    s_batch.vb = s_batch.ib = s_batch.ub = 0;
    s_batch.rc = 0;
    if (s_batch.pool && s_batch.sets) pvkResetDescriptorPool(s_vk.device, s_batch.pool, 0);
    s_batch.sets = 0;
    vhp_record(drawn, hp_begin, hp_queued, hp_waited, (int)r, site);
    s_batch.submits++;
    s_batch.batched += drawn;
    if (s_batch.stats && s_batch.draws - s_batch.last_report >= 20000) {
        s_batch.last_report = s_batch.draws;
        VK_LOG("batch: submits=%llu draws=%llu batched=%llu merged=%llu uploads=%llu flush entry=%llu destroy=%llu ring=%llu fallback=%llu\n",
               (unsigned long long)s_batch.submits, (unsigned long long)s_batch.draws,
               (unsigned long long)s_batch.batched, (unsigned long long)s_batch.merged,
               (unsigned long long)s_batch.deferred_uploads,
               (unsigned long long)s_batch.flush[0],
               (unsigned long long)s_batch.flush[1], (unsigned long long)s_batch.flush[2],
               (unsigned long long)s_batch.flush[3]);
    }
    if (r != VK_SUCCESS) { VK_LOG("submit_and_wait failed (VkResult %d)\n", (int)r); return -1; }
    return 0;
}

#define vk_submit_and_wait() vk_submit_and_wait_impl(__func__)

/* Submit and wait for batched draws, if any.
 * reason: 0 entry point, 1 object destruction, 2 ring/pool full, 3 fallback. */
static void vk_flush_pending_impl(int reason, const char* site)
{
    if (!s_batch.recording || !(s_batch.pending || s_batch.deferred)) return;
    if (reason >= 0 && reason < 4) s_batch.flush[reason]++;
    if (s_batch.trace) { s_batch.trace--; VK_LOG("batch-trace: flush reason=%d pending=%u\n", reason, s_batch.pending); }
    vk_submit_and_wait_impl(site);
}

/* End of a command that only the GPU depends on (a clear, a layout change):
 * with deferral it stays recorded behind the batch, else it is submitted and
 * waited on as before. */
#define vk_submit_or_defer() (s_batch.defer ? (s_batch.deferred++, 0) : vk_submit_and_wait())

/* Order this command buffer's use of an image after everything submitted
 * before it; from UNDEFINED it is also the one-time move to GENERAL. */
static void vk_barrier_image(VkImage img, VkImageAspectFlags aspect, VkImageLayout old_layout)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = old_layout, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS },
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

#include "rsx_vulkan_fxaa.inc"
#include "rsx_vulkan_fsr1.inc"

/* ---------------------------------------------------------------------------
 * Texture unit 0
 * -------------------------------------------------------------------------*/
static void vk_write_texture_descriptor(void)
{
    VkDescriptorImageInfo ii = { .sampler = s_vk.sampler,
                                 .imageView = s_vk.tex_ready[0] ? s_vk.tex[0].view
                                                                : s_vk.dummy.view,
                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = s_vk.desc_set, .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii };
    pvkUpdateDescriptorSets(s_vk.device, 1, &w, 0, NULL);
}

/* Replace *dst with an RGBA8 image of `nlv` levels: level m is lw[m] x lh[m]
 * texels, tightly packed, the levels one after another in `rgba`. */
static int vk_upload_texture_levels(vk_image* dst, const u8* rgba,
                                    const u32* lw, const u32* lh, u32 nlv)
{
    VkBufferImageCopy regions[RSX_MAX_TEXTURE_LEVELS];
    if (!nlv || nlv > RSX_MAX_TEXTURE_LEVELS) return -1;
    VkDeviceSize total = 0;
    for (u32 m = 0; m < nlv; m++) {
        regions[m] = (VkBufferImageCopy){
            .bufferOffset = total,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 },
            .imageExtent = { lw[m], lh[m], 1 },
        };
        total += (VkDeviceSize)lw[m] * lh[m] * 4u;
    }
    vk_buffer staging = {0};
    if (vk_create_host_buffer(&staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !staging.coherent) {
        vk_destroy_buffer(&staging); return -1;
    }
    memcpy(staging.ptr, rgba, (size_t)total);

    vk_image fresh = {0};
    if (vk_create_image_levels(&fresh, VK_FORMAT_R8G8B8A8_UNORM, lw[0], lh[0], nlv,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&fresh); vk_destroy_buffer(&staging); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(fresh.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        pvkCmdCopyBufferToImage(s_vk.cmd, staging.buf, fresh.img, VK_IMAGE_LAYOUT_GENERAL,
                                nlv, regions);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&staging);
    if (rc) { vk_destroy_image(&fresh); return -1; }

    vk_destroy_image(dst);          /* idle: every submission is waited on */
    *dst = fresh;
    return 0;
}

static int vk_upload_texture(vk_image* dst, const u8* rgba, u32 w, u32 h)
{
    return vk_upload_texture_levels(dst, rgba, &w, &h, 1);
}

/* NV4097_SET_TEXTURE_ADDRESS wrap field, one per axis -- the table the Metal
 * backend and the live draw engine use: 1 WRAP, 2 MIRROR, 3 CLAMP_TO_EDGE,
 * 4 BORDER, 5 CLAMP, 6..8 MIRROR_ONCE. Vulkan 1.0 has MIRROR_CLAMP_TO_EDGE
 * only behind an extension, so the MIRROR_ONCE family clamps to edge here --
 * exact for texcoords in [0,1], the mirrored half is what is lost. */
static VkSamplerAddressMode vk_gcm_wrap(u32 w)
{
    switch (w & 0xFu) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;      /* 3, 5..8, unset */
    }
}

/* A unit's sampler from its registers, decoded as the Metal backend's
 * samp_slot_for does: SET_TEXTURE_FILTER min at [18:16] (1 NEAREST,
 * 2 LINEAR, 3..6 the four nearest/linear x nearest/linear-mip pairs), mag at
 * [26:24]; SET_TEXTURE_CONTROL0 max LOD at [18:7], min LOD at [30:19], 4.8
 * fixed point. The LOD bias (FILTER [12:0]) is not applied, as in Metal.
 * VK_NULL_HANDLE on failure: the caller falls back to `sampler`. */
static VkSampler vk_unit_sampler(const rsx_texture_state* t)
{
    const u32 minf = (t->filter >> 16) & 7u, magf = (t->filter >> 24) & 7u;
    const u32 lod  = (t->control0 >> 7) & 0xFFFFFFu;       /* max then min LOD */
    const u64 key  = (u64)minf | ((u64)magf << 3)
                   | ((u64)(t->address & 0x000F0F0Fu) << 6) | ((u64)lod << 26)
                   | ((u64)((t->control0 >> 4) & 7u) << 50);
    for (u32 i = 0; i < s_vk.samp_count; i++)
        if (s_vk.samp_cache[i].key == key) return s_vk.samp_cache[i].s;
    if (s_vk.samp_count >= VK_SAMPLER_CACHE) return VK_NULL_HANDLE;

    const int mip_present = (minf >= 3);
    const int mip_linear  = (minf == 5 || minf == 6);
    /* A min filter with no mip term samples level 0 only: pin the range. */
    const float min_lod = (float)((t->control0 >> 19) & 0xFFFu) / 256.0f;
    float max_lod = mip_present ? (float)((t->control0 >> 7) & 0xFFFu) / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (magf == 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (minf == 2 || minf == 4 || minf == 6) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(t->address),
        .addressModeV = vk_gcm_wrap(t->address >> 8),
        .addressModeW = vk_gcm_wrap(t->address >> 16),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    rsx_vk_anisotropy_apply(&sci, (t->control0 >> 4) & 7u, s_vk.anisotropy_override,
                             s_vk.anisotropy_ok, s_vk.max_anisotropy, 1);
    VkSampler smp = VK_NULL_HANDLE;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return VK_NULL_HANDLE;
    s_vk.samp_cache[s_vk.samp_count].key = key;
    s_vk.samp_cache[s_vk.samp_count].s   = smp;
    s_vk.samp_count++;
    return smp;
}

/* One level of a guest texture into tightly packed RGBA8, with the null
 * backend's decode (rsx_null_backend.c, nullsw_bind_texture): formats that
 * decode to fewer than four bytes are splayed, alpha forced opaque. */
static int vk_decode_level_rgba8(u8* dst, const u8* src, u32 w, u32 h,
                                 const rsx_tex_layout* tl)
{
    const u32 pitch = w * 4u;
    if (tl->fmt == RSX_TEXFMT_R8G8B8A8) {
        rsx_texture_decode(dst, pitch, src, w, h, tl, rsx_texture_argb_is_rgba());
        return 0;
    }
    const u32 srcp = tl->row_bytes;
    u8* tmp = (u8*)malloc((size_t)srcp * h);
    if (!tmp) return -1;
    rsx_texture_decode(tmp, srcp, src, w, h, tl, 0);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u8* sp = tmp + (size_t)y * srcp + (size_t)x * tl->bytes_per_texel;
            u8* dp = dst + (size_t)y * pitch + (size_t)x * 4u;
            dp[0] = sp[0];
            dp[1] = tl->bytes_per_texel > 1 ? sp[1] : sp[0];
            dp[2] = tl->bytes_per_texel > 2 ? sp[2] : sp[0];
            dp[3] = 255;
        }
    free(tmp);
    return 0;
}

/* Unit `unit` has nothing usable bound. The fallback path's descriptor only
 * ever shows unit 0, so only that one needs rewriting. */
static void vk_tex_off(u32 unit)
{
    s_vk.tex_ready[unit] = 0;
    s_vk.tex_sampler[unit] = VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* Same decode as the headless null backend (rsx_null_backend.c,
 * nullsw_bind_texture), so both backends sample identical texels. */
static void vk_cb_bind_texture(void* ud, u32 unit, const rsx_texture_state* t)
{ vk_flush_pending(0);
    (void)ud;
    if (unit >= RSX_MAX_TEXTURES || !t || !s_vk.device) return;
    if (!(t->control0 & 0x80000000u) || !t->offset) { vk_tex_off(unit); return; }

    u32 w = (t->image_rect >> 16) & 0xFFFFu;
    u32 h =  t->image_rect        & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) { vk_tex_off(unit); return; }

    u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (!vm_base || ea == 0xFFFFFFFFu) { vk_tex_off(unit); return; }
    const u32 fmt = (t->format >> 8) & 0xFFu;

    /* The whole mip chain, as the Metal backend reads it: SET_TEXTURE_FORMAT's
     * level count above bit 15 and SET_TEXTURE_CONTROL3's row pitch, laid out
     * by the shared rsx_texture_mip_chain() (which clamps the count to what
     * the dimensions allow). Cube maps are not handled yet: face 0 only. */
    const u32 levels    = (t->format >> 16) & 0xFFFFu;
    const u32 row_pitch = t->control3 & 0xFFFFFu;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, row_pitch, lv);
    if (!nlv || lv[0].tl.compressed) { vk_tex_off(unit); return; }   /* no BC path yet */

    size_t total = 0, off[RSX_MAX_TEXTURE_LEVELS];
    u32 lw[RSX_MAX_TEXTURE_LEVELS], lh[RSX_MAX_TEXTURE_LEVELS];
    for (u32 m = 0; m < nlv; m++) {
        off[m] = total;
        lw[m] = lv[m].w; lh[m] = lv[m].h;
        total += (size_t)lv[m].w * lv[m].h * 4u;
    }
    u8* buf = (u8*)malloc(total);
    if (!buf) { vk_tex_off(unit); return; }
    for (u32 m = 0; m < nlv; m++)
        if (vk_decode_level_rgba8(buf + off[m], vm_base + ea + lv[m].offset,
                                  lv[m].w, lv[m].h, &lv[m].tl)) {
            free(buf); vk_tex_off(unit); return;
        }
    s_vk.tex_ready[unit] = vk_upload_texture_levels(&s_vk.tex[unit], buf, lw, lh, nlv) == 0;
    free(buf);
    s_vk.tex_sampler[unit] = s_vk.tex_ready[unit] ? vk_unit_sampler(t) : VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* ---------------------------------------------------------------------------
 * Render pass, framebuffer, pipelines
 * -------------------------------------------------------------------------*/
static int vk_pick_depth_format(void)
{
    /* With stencil first -- titles use it for shadows, reflections and UI
     * masks -- and D32 first within each group: a D32 depth aspect copies out
     * as 32-bit floats, which is what depth snapshots are built from. */
    static const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM
    };
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if ((fp.optimalTilingFeatures & need) == need) {
            s_vk.depth_format = candidates[i]; goto chosen;
        }
    }
    /* TRANSFER_DST is a 1.1 format feature bit; 1.0 drivers may not report
     * it although clears work. Retry on the attachment bit alone. */
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            s_vk.depth_format = candidates[i]; goto chosen;
        }
    }
    VK_LOG("no usable depth format\n");
    return -1;
chosen:
    s_vk.depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (s_vk.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        s_vk.depth_format == VK_FORMAT_D24_UNORM_S8_UINT)
        s_vk.depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return 0;
}

static int vk_create_render_pass(void)
{
    VkAttachmentDescription att[2] = {
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .format = s_vk.depth_format, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,     /* ignored without stencil */
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 2, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    VK_CHECK(pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_vk.render_pass), "vkCreateRenderPass");

    VkImageView views[2] = { s_vk.color.view, s_vk.depth.view };
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = s_vk.render_pass, .attachmentCount = 2,
                                    .pAttachments = views, .width = s_vk.width,
                                    .height = s_vk.height, .layers = 1 };
    VK_CHECK(pvkCreateFramebuffer(s_vk.device, &fci, NULL, &s_vk.framebuffer), "vkCreateFramebuffer");
    return 0;
}

static int vk_create_shader(const uint32_t* code, size_t bytes, VkShaderModule* out)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = bytes, .pCode = code };
    VK_CHECK(pvkCreateShaderModule(s_vk.device, &ci, NULL, out), "vkCreateShaderModule");
    return 0;
}

static int vk_create_pipeline_objects(void)
{
    if (vk_create_shader(rsx_vk_fallback_vert_spv, sizeof rsx_vk_fallback_vert_spv, &s_vk.vs) ||
        vk_create_shader(rsx_vk_fallback_frag_spv, sizeof rsx_vk_fallback_frag_spv, &s_vk.fs))
        return -1;

    VkDescriptorSetLayoutBinding bind = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &bind };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_vk.set_layout),
             "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_vk.desc_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_vk.desc_pool,
                                        .descriptorSetCount = 1, .pSetLayouts = &s_vk.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_vk.desc_set), "vkAllocateDescriptorSets");

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_vk.set_layout,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_vk.pipe_layout),
             "vkCreatePipelineLayout");

    /* Nearest + repeat: the null backend's point sample with wrapping. */
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .maxLod = 0.0f };
    VK_CHECK(pvkCreateSampler(s_vk.device, &sci, NULL, &s_vk.sampler), "vkCreateSampler");

    /* A 1x1 magenta dummy keeps the descriptor valid before any bind --
     * the same "not bound" colour the null backend's sampler returns. */
    static const u8 magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
    if (vk_upload_texture(&s_vk.dummy, magenta, 1, 1)) return -1;
    vk_write_texture_descriptor();
    return 0;
}

/* NV4097 depth function is the GL enum 0x200 NEVER .. 0x207 ALWAYS, which is
 * VkCompareOp 0..7 in the same order. Anything else passes, as in the null
 * and D3D12 backends (including the 1 rsx_state_init seeds). */
static u32 vk_compare_slot(u32 gl_func)
{
    return (gl_func >= 0x200u && gl_func <= 0x207u) ? gl_func - 0x200u
                                                    : (u32)VK_COMPARE_OP_ALWAYS;
}

static VkPipeline vk_get_pipeline(int ztest, int zwrite, u32 cmp)
{
    u32 slot = ((u32)ztest * 2u + (u32)zwrite) * 8u + cmp;
    if (s_vk.pipelines[slot]) return s_vk.pipelines[slot];

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_vk.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_vk.fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, sizeof(vk_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, pos) },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, col) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(vk_vertex, uv)  },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* No culling: the null backend fills both windings. */
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_vk.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vk_create_graphics_timed("fallback", 0, s_vk.device, vk_pcache(), 1, &gci, NULL, &p);
    if (r != VK_SUCCESS) { VK_LOG("vkCreateGraphicsPipelines failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
    vk_pcache_created();
    s_vk.pipelines[slot] = p;
    return p;
}

/* ---------------------------------------------------------------------------
 * Draw path
 * -------------------------------------------------------------------------*/
static void vk_cb_track_state(void* ud, const rsx_state* state)
{ vk_flush_pending(0);
    (void)ud; s_vk.state = state;
}

static int vk_push_vertex(const float pos[4], const float col[4], const float uv[4])
{
    if (s_vk.cpu_count == s_vk.cpu_cap) {
        size_t cap = s_vk.cpu_cap ? s_vk.cpu_cap * 2 : 1024;
        vk_vertex* n = (vk_vertex*)realloc(s_vk.cpu_verts, cap * sizeof *n);
        if (!n) return -1;
        s_vk.cpu_verts = n; s_vk.cpu_cap = cap;
    }
    vk_vertex* v = &s_vk.cpu_verts[s_vk.cpu_count++];
    memcpy(v->pos, pos, sizeof v->pos);
    memcpy(v->col, col, sizeof v->col);
    v->uv[0] = uv[0]; v->uv[1] = uv[1];
    return 0;
}

/* One triangle, colour from its first vertex (the null backend's flat shade). */
static void vk_emit_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    float p[3][4], uv[3][4], col[4];
    rsx_fetch_attrib(st, 0, i0, p[0]);
    rsx_fetch_attrib(st, 0, i1, p[1]);
    rsx_fetch_attrib(st, 0, i2, p[2]);
    rsx_fetch_attrib(st, 3, i0, col);
    rsx_fetch_attrib(st, 8, i0, uv[0]);
    rsx_fetch_attrib(st, 8, i1, uv[1]);
    rsx_fetch_attrib(st, 8, i2, uv[2]);
    for (int k = 0; k < 3; k++) vk_push_vertex(p[k], col, uv[k]);
}

/* Called once per expanded triangle: the fixed-function path records the
 * null backend's flat-shaded vertex, the guest path all sixteen attributes. */
typedef void (*vk_tri_fn)(const rsx_state* st, u32 i0, u32 i1, u32 i2);

/* Primitive expansion, identical to the null backend's draw_prim(). */
static void vk_expand(const rsx_state* st, u32 prim, u32 first, u32 count, vk_tri_fn emit)
{
    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i + 2 < count; i += 3) emit(st, first + i, first + i + 1, first + i + 2);
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; i++)
            emit(st, first + i, first + i + 1 + (i & 1), first + i + 2 - (i & 1));
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; i++) emit(st, first, first + i, first + i + 1);
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            emit(st, first + i, first + i + 1, first + i + 2);
            emit(st, first + i, first + i + 2, first + i + 3);
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            emit(st, first + i, first + i + 1, first + i + 3);
            emit(st, first + i, first + i + 3, first + i + 2);
        }
        break;
    default:
        break;   /* points and lines: no coverage, as in the null backend */
    }
}

static int vk_ensure_vertex_buffer(VkDeviceSize bytes)
{
    if (s_vk.vertices.buf && s_vk.vertices.size >= bytes) return 0;
    VkDeviceSize size = s_vk.vertices.size ? s_vk.vertices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_vk.vertices);   /* idle: every submission is waited on */
    if (vk_create_host_buffer(&s_vk.vertices, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return -1;
    if (!s_vk.vertices.coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Guest programs (stage V3)
 *
 * With the HLSL -> SPIR-V translator built and PS3RECOMP_VK_GUEST_PROGRAMS=1,
 * a draw runs the guest's own vertex and fragment programs, as the Metal
 * backend's guest path does and keyed the same way: decompiled to HLSL by the
 * shared decompilers, translated by rsx_shader_spirv.cpp, cached per program.
 * Constants use the Metal/D3D12 layout: VPConst is the 512 transform
 * constants plus the viewport epilogue, PSConstants the fragment program's
 * inline constants plus fp_alpha.
 *
 * Vertices are still fetched and expanded on the CPU through the shared
 * rsx_fetch_attrib(), now all sixteen attributes (a disabled one reads its
 * constant register, as on the hardware). Every RSX vertex format therefore
 * arrives as float4, and the pipeline has one fixed input layout: location n
 * is attribute n, which is what the vertex decompiler declares.
 *
 * A program that needs a cube map or a vertex texture falls back to the
 * fixed-function path, with a message. Not honoured on this path yet: the RSX
 * viewport and scissor (the whole target is used, as on the fallback path),
 * blending, stencil, culling, per-unit sampler state.
 * -------------------------------------------------------------------------*/
#define VK_VP_CACHE      256
#define VK_FP_CACHE      512
#define VK_GPIPE_CACHE   512
#define VK_FP_MAX_BYTES  4096u              /* bound on a fragment program, as Metal/D3D12 */
#define VK_HLSL_BYTES    (256u * 1024u)
#define VK_SPV_WORDS     (256u * 1024u)
#define VK_VPCONST_BYTES ((RSX_MAX_VERTEX_CONSTANTS + 2u) * 16u)
#define VK_GUEST_ATTRS   16u
#define VK_GUEST_FLOATS  (VK_GUEST_ATTRS * 4u)   /* per vertex */

typedef struct { u32 hash; VkShaderModule mod; } vk_vp_entry;
typedef struct { u64 key; VkShaderModule mod; u32 nconst; } vk_fp_entry;
typedef struct { int vs, fs; u32 depth_key; VkPipeline pipe; } vk_gpipe_entry;

static struct {
    int                   on;
    char*                 hlsl;
    u32*                  spv;
    char                  log[4096];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout      pipe_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;
    vk_buffer             ubo;              /* VPConst at 0, PSConstants at ps_off */
    VkDeviceSize          ps_off;
    VkDeviceSize          max_ubo_range;
    vk_buffer             vertices;
    float*                cpu;
    size_t                cpu_cap, cpu_count;   /* in vertices */
    vk_vp_entry           vp[VK_VP_CACHE];     u32 vp_count;
    vk_fp_entry           fp[VK_FP_CACHE];     u32 fp_count;
    vk_gpipe_entry        pipes[VK_GPIPE_CACHE]; u32 pipe_count;
    rsx_fp_constant_block consts;
    u32                   draws, last_draws;
    u32                   warned;
} s_g;

int rsx_vulkan_backend_guest_programs(void)
{
    const char* v = getenv("PS3RECOMP_VK_GUEST_PROGRAMS");
    return rsx_hlsl_to_spirv_available() && v && v[0] && v[0] != '0';
}

static u32 vk_fnv1a32(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u64 vk_fnv1a64(const void* d, size_t n, u64 h)
{
    const u8* p = (const u8*)d;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* Built from the registers, as the Metal and D3D12 backends do. */
static u32 vk_cube_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->textures[u];
        const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
        if ((t->control0 & 0x80000000u) && (t->format & 4u) && w && w == h) m |= 1u << u;
    }
    return m;
}

static u32 vk_vtex_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++)
        if (st->vertex_textures[u].control0 & 0x80000000u) m |= 1u << u;
    return m;
}

static VkShaderModule vk_guest_module(const char* what, int stage)
{
    u32 n = 0;
    if (rsx_hlsl_to_spirv(s_g.hlsl, stage, s_g.spv, VK_SPV_WORDS, &n,
                          s_g.log, sizeof s_g.log) != 0) {
        VK_LOG("%s: %s\n", what, s_g.log);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)n * 4u, .pCode = s_g.spv };
    VkShaderModule m = VK_NULL_HANDLE;
    if (pvkCreateShaderModule(s_vk.device, &ci, NULL, &m) != VK_SUCCESS) {
        VK_LOG("%s: vkCreateShaderModule failed\n", what);
        return VK_NULL_HANDLE;
    }
    return m;
}

/* The vertex program, keyed on its own bytes to its end bit (as Metal). */
static int vk_vp_slot(const rsx_state* st)
{
    if (st->vp_ucode_bytes < 16) return -1;
    u32 vstart = st->transform_program_start * 16u;
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* uc = st->vp_ucode + vstart;
    const u32 avail = st->vp_ucode_bytes - vstart;
    const u32 instrs = rsx_vp_program_size_instrs(uc, avail);
    const u32 len = instrs ? instrs * 16u : avail;
    const u32 hash = vk_fnv1a32(uc, len);
    for (u32 i = 0; i < s_g.vp_count; i++)
        if (s_g.vp[i].hash == hash) return s_g.vp[i].mod ? (int)i : -1;
    if (s_g.vp_count >= VK_VP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "vertex program %08X", hash);
    VkShaderModule m = VK_NULL_HANDLE;
    const int ni = rsx_vp_decompile_ex(uc, len, 0, s_g.hlsl, VK_HLSL_BYTES);
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_VERTEX);
    if (s_g.vp_count < 32)
        VK_LOG("%s: %d instrs -> %s\n", what, ni, m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.vp_count++;
    s_g.vp[slot].hash = hash;
    s_g.vp[slot].mod  = m;
    return m ? slot : -1;
}

/* The fragment program, resolved and keyed as the Metal backend's
 * fp_slot_for: structure (inline constants live in PSConstants), export
 * width, and the alpha test patched into the source. No cube units here --
 * the caller has already sent those to the fixed-function path. */
static int vk_fp_slot(const rsx_state* st, const u8** uc)
{
    if (!vm_base || st->shader_program == 0) return -1;
    const u32 off = cellGcmResolveLocated((st->shader_program & 3u) == 1u,
                                          st->shader_program & ~3u);
    if (off == 0xFFFFFFFFu) return -1;
    *uc = vm_base + off;
    u64 key = rsx_fp_structural_hash(*uc, VK_FP_MAX_BYTES, 1469598103934665603ull);
    if (!key) return -1;                       /* malformed or unterminated */
    const u32 ctrl     = st->shader_control;
    const u32 ctrl_key = ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    const u32 alpha_en = (st->alpha_test_enable && st->alpha_func != 0x0207u) ? 1u : 0u;
    const u32 alpha_fn = alpha_en ? st->alpha_func : 0u;
    key = vk_fnv1a64(&ctrl_key, sizeof ctrl_key, key);
    key = vk_fnv1a64(&alpha_en, sizeof alpha_en, key);
    key = vk_fnv1a64(&alpha_fn, sizeof alpha_fn, key);
    for (u32 i = 0; i < s_g.fp_count; i++)
        if (s_g.fp[i].key == key) return s_g.fp[i].mod ? (int)i : -1;
    if (s_g.fp_count >= VK_FP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "fragment program %016llX", (unsigned long long)key);
    VkShaderModule m = VK_NULL_HANDLE;
    u32 nconst = 0;
    int ni = rsx_fp_decompile_buffered_ex(*uc, VK_FP_MAX_BYTES, ctrl, 0,
                                          s_g.hlsl, VK_HLSL_BYTES, &nconst);
    if (ni > 0 && alpha_en &&
        rsx_fp_apply_alpha_test_buffered(s_g.hlsl, VK_HLSL_BYTES, st->alpha_func) < 0)
        ni = -1;
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_FRAGMENT);
    if (s_g.fp_count < 32)
        VK_LOG("%s: %d instrs, %u constants -> %s\n", what, ni, nconst,
               m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.fp_count++;
    s_g.fp[slot].key    = key;
    s_g.fp[slot].mod    = m;
    s_g.fp[slot].nconst = nconst;
    return m ? slot : -1;
}

static VkPipeline vk_guest_pipeline(int vs, int fs, int ztest, int zwrite, u32 cmp)
{
    const u32 dkey = (u32)ztest | ((u32)zwrite << 1) | (cmp << 2);
    for (u32 i = 0; i < s_g.pipe_count; i++)
        if (s_g.pipes[i].vs == vs && s_g.pipes[i].fs == fs && s_g.pipes[i].depth_key == dkey)
            return s_g.pipes[i].pipe;
    if (s_g.pipe_count >= VK_GPIPE_CACHE) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_g.vp[vs].mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_g.fp[fs].mod, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, VK_GUEST_FLOATS * 4u, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) {
        va[a].location = a;
        va[a].binding  = 0;
        va[a].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[a].offset   = a * 16u;
    }
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = VK_GUEST_ATTRS, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_g.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vk_create_graphics_timed("guest", s_g.pipe_count, s_vk.device, vk_pcache(), 1, &gci, NULL, &p);
    if (r != VK_SUCCESS) {
        VK_LOG("guest pipeline: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
        return VK_NULL_HANDLE;
    }
    vk_pcache_created();
    vk_gpipe_entry* e = &s_g.pipes[s_g.pipe_count++];
    e->vs = vs; e->fs = fs; e->depth_key = dkey; e->pipe = p;
    return p;
}

/* One vertex: all sixteen attributes as float4, attribute n at float 4n. */
static int vk_guest_push(const rsx_state* st, u32 idx)
{
    if (s_g.cpu_count == s_g.cpu_cap) {
        size_t cap = s_g.cpu_cap ? s_g.cpu_cap * 2 : 1024;
        float* n = (float*)realloc(s_g.cpu, cap * VK_GUEST_FLOATS * sizeof(float));
        if (!n) return -1;
        s_g.cpu = n; s_g.cpu_cap = cap;
    }
    float* v = s_g.cpu + s_g.cpu_count * VK_GUEST_FLOATS;
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) rsx_fetch_attrib(st, (int)a, idx, v + a * 4u);
    s_g.cpu_count++;
    return 0;
}

static void vk_guest_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    vk_guest_push(st, i0);
    vk_guest_push(st, i1);
    vk_guest_push(st, i2);
}

static int vk_guest_init(void)
{
    if (!rsx_vulkan_backend_guest_programs()) return 0;
    s_g.hlsl = (char*)malloc(VK_HLSL_BYTES);
    s_g.spv  = (u32*)malloc(VK_SPV_WORDS * sizeof(u32));
    if (!s_g.hlsl || !s_g.spv) { VK_LOG("guest programs: out of memory\n"); return -1; }

    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_g.max_ubo_range = p.limits.maxUniformBufferRange;
    VkDeviceSize align = p.limits.minUniformBufferOffsetAlignment;
    if (align == 0) align = 256;
    s_g.ps_off = (VK_VPCONST_BYTES + align - 1u) / align * align;
    if (VK_VPCONST_BYTES > s_g.max_ubo_range) {
        VK_LOG("guest programs: maxUniformBufferRange %llu < VPConst\n",
               (unsigned long long)s_g.max_ubo_range);
        return -1;
    }

    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[4] = {
        { RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { RSX_SPIRV_TEXTURE_BINDING, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4, .pBindings = b };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_g.set_layout),
             "vkCreateDescriptorSetLayout(guest)");
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_g.pool),
             "vkCreateDescriptorPool(guest)");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_g.pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_g.set), "vkAllocateDescriptorSets(guest)");
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_g.pipe_layout),
             "vkCreatePipelineLayout(guest)");
    if (vk_create_host_buffer(&s_g.ubo,
                              s_g.ps_off + (RSX_FP_MAX_INLINE_CONSTANTS + 1u) * 16u,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_g.ubo.coherent) {
        VK_LOG("guest programs: no host-coherent uniform memory\n");
        return -1;
    }
    s_g.on = 1;
    VK_LOG("guest programs: on (HLSL -> SPIR-V through glslang)\n");
    return 0;
}

static void vk_guest_shutdown(void)
{
    if (s_vk.device) {
        for (u32 i = 0; i < s_g.pipe_count; i++)
            if (s_g.pipes[i].pipe) pvkDestroyPipeline(s_vk.device, s_g.pipes[i].pipe, NULL);
        for (u32 i = 0; i < s_g.vp_count; i++)
            if (s_g.vp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.vp[i].mod, NULL);
        for (u32 i = 0; i < s_g.fp_count; i++)
            if (s_g.fp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.fp[i].mod, NULL);
        if (s_g.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_g.pipe_layout, NULL);
        if (s_g.pool)        pvkDestroyDescriptorPool(s_vk.device, s_g.pool, NULL);
        if (s_g.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_g.set_layout, NULL);
        vk_destroy_buffer(&s_g.ubo);
        vk_destroy_buffer(&s_g.vertices);
    }
    free(s_g.hlsl);
    free(s_g.spv);
    free(s_g.cpu);
    memset(&s_g, 0, sizeof s_g);
}

static int vk_ensure_buffer(vk_buffer* b, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    if (b->buf && b->size >= bytes) return 0;
    VkDeviceSize size = b->size ? b->size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(b);                 /* idle: every submission is waited on */
    if (vk_create_host_buffer(b, size, usage)) return -1;
    if (!b->coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}


/* Returns 1 when the draw went through the guest programs (or had nothing to
 * draw), 0 when the caller must use the fixed-function path instead. */
static int vk_guest_draw(const rsx_state* st, const rsx_depth_clip* depth_clip,
                         u32 prim, u32 first, u32 count)
{
    if (!s_g.on) return 0;
    if (vk_cube_mask(st) || vk_vtex_mask(st)) {
        if (!(s_g.warned & 1u)) {
            VK_LOG("guest programs: cube maps and vertex textures are not supported "
                   "yet; those draws use the fixed-function path\n");
            s_g.warned |= 1u;
        }
        return 0;
    }
    const int vs = vk_vp_slot(st);
    if (vs < 0) return 0;
    const u8* fp_uc = NULL;
    const int fs = vk_fp_slot(st, &fp_uc);
    if (fs < 0) return 0;
    /* The constant count must be what the program was compiled against. */
    if (rsx_fp_collect_constants(fp_uc, VK_FP_MAX_BYTES, &s_g.consts) < 0 ||
        s_g.consts.count != s_g.fp[fs].nconst)
        return 0;
    const u32 nslots = s_g.consts.count ? s_g.consts.count : 1u;
    const u32 fp_len = (nslots + 1u) * 16u;
    if (fp_len > s_g.max_ubo_range) return 0;

    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_guest_pipeline(vs, fs, ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return 0;

    s_g.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_guest_tri);
    if (!s_g.cpu_count) return 1;
    const VkDeviceSize bytes = (VkDeviceSize)s_g.cpu_count * VK_GUEST_FLOATS * sizeof(float);
    if (vk_ensure_buffer(&s_g.vertices, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
    memcpy(s_g.vertices.ptr, s_g.cpu, (size_t)bytes);

    /* VPConst: the 512 transform constants, then the viewport epilogue.
     * Normalize guest window depth into host clip space; the paired host
     * viewport restores the original guest depth for testing and storage. */
    u8* vpc = (u8*)s_g.ubo.ptr;
    memcpy(vpc, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16u);
    float* vpx = (float*)(vpc + RSX_MAX_VERTEX_CONSTANTS * 16u);
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    vpx[2] = depth_clip->scale;
    vpx[6] = depth_clip->offset;
    /* PSConstants: inline constants as host-order bit patterns, then fp_alpha. */
    u8* fpc = (u8*)s_g.ubo.ptr + s_g.ps_off;
    memset(fpc, 0, fp_len);
    if (s_g.consts.count) memcpy(fpc, s_g.consts.values, s_g.consts.count * 16u);
    float* alpha = (float*)(fpc + nslots * 16u);
    alpha[0] = rsx_fp_alpha_ref(st->alpha_ref, st->surface_format & 0x1Fu);

    /* Descriptors: rewritten per draw, which is safe because every submission
     * is waited on before the next one is recorded. */
    VkDescriptorBufferInfo bi[2] = {
        { s_g.ubo.buf, 0, VK_VPCONST_BYTES },
        { s_g.ubo.buf, s_g.ps_off, fp_len },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
                                         s_vk.tex_ready[u] ? s_vk.tex[u].view : s_vk.dummy.view,
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ s_vk.tex_sampler[u] ? s_vk.tex_sampler[u] : s_vk.sampler,
                                         VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si },
    };
    pvkUpdateDescriptorSets(s_vk.device, 4, w, 0, NULL);

    if (vk_begin()) return 0;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vpt = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height,
                       depth_clip->min_depth, depth_clip->max_depth };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vpt);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_g.pipe_layout,
                             0, 1, &s_g.set, 0, NULL);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_g.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_g.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    if (vk_submit_and_wait()) return 0;
    s_g.draws++;
    return 1;
}

static void vk_draw(u32 prim, u32 first, u32 count)
{
    const rsx_state* st = s_vk.state;
    if (!st || !s_vk.device || count < 3) return;
    rsx_depth_clip depth_clip;
    if (st->clip_control != 1u ||
        rsx_depth_clip_build(st->clip_min, st->clip_max,
                             st->viewport_scale[2], st->viewport_offset[2],
                             &depth_clip)) {
        ps3_strict_boot_guard("rsx-depth-clip-unsupported",
                              "control=0x%08X clip=[%g,%g] z=[%g,%g]",
                              st->clip_control, st->clip_min, st->clip_max,
                              st->viewport_scale[2], st->viewport_offset[2]);
        return;
    }
    if (vk_guest_draw(st, &depth_clip, prim, first, count)) return;

    s_vk.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_emit_tri);
    if (!s_vk.cpu_count) return;

    /* The fixed shader consumes clip-space attribute 0 directly. Apply the
     * same Z mapping on the CPU before that shader runs. Its w==0 fallback
     * uses one, so use the same W here for the offset term. */
    for (size_t i = 0; i < s_vk.cpu_count; i++) {
        vk_vertex* v = &s_vk.cpu_verts[i];
        const float w = v->pos[3] != 0.0f ? v->pos[3] : 1.0f;
        v->pos[2] = depth_clip.scale * v->pos[2] + depth_clip.offset * w;
    }

    const VkDeviceSize bytes = (VkDeviceSize)s_vk.cpu_count * sizeof(vk_vertex);
    if (vk_ensure_vertex_buffer(bytes)) return;
    memcpy(s_vk.vertices.ptr, s_vk.cpu_verts, (size_t)bytes);

    /* Depth writes need the test enabled, as on the hardware and in both
     * reference backends. */
    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_get_pipeline(ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return;
    const int32_t textured = (s_vk.tex_ready[0] && st->vertex_attribs[8].enabled) ? 1 : 0;

    if (vk_begin()) return;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    /* Whole target, as the null backend's ndc_to_px maps it. */
    VkViewport vp = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height,
                      depth_clip.min_depth, depth_clip.max_depth };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.pipe_layout,
                             0, 1, &s_vk.desc_set, 0, NULL);
    pvkCmdPushConstants(s_vk.cmd, s_vk.pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof textured, &textured);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_vk.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_vk.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_submit_and_wait();
}

static void vk_cb_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{ vk_flush_pending(0);
    (void)ud;
    vk_draw(primitive, first, count);
}

static void vk_cb_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{ vk_flush_pending(0);
    /* Not read yet, as in the null backend: the harness geometry is
     * non-indexed. Real titles need this; it lands with the index fetch. */
    (void)ud; (void)primitive; (void)offset; (void)count;
}

/* ---------------------------------------------------------------------------
 * Clear and present
 * -------------------------------------------------------------------------*/
static void vk_cb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{ vk_flush_pending(0);
    (void)ud; (void)stencil;                       /* no stencil buffer yet */
    s_vk.clear_argb = color;
    /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 the colour channels
     * (all or nothing, as in the null backend). */
    const int do_color = (flags & 0xF0u) != 0;
    const int do_depth = (flags & 0x01u) != 0;
    if (!s_vk.device || (!do_color && !do_depth)) return;

    if (vk_begin()) return;
    if (do_color) {
        VkClearColorValue v;
        v.float32[0] = (float)((color >> 16) & 0xFFu) / 255.0f;   /* R */
        v.float32[1] = (float)((color >>  8) & 0xFFu) / 255.0f;   /* G */
        v.float32[2] = (float)( color        & 0xFFu) / 255.0f;   /* B */
        v.float32[3] = (float)((color >> 24) & 0xFFu) / 255.0f;   /* A */
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    if (do_depth) {
        VkClearDepthStencilValue d = { depth, 0 };
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    }
    vk_submit_and_wait();
}

/* ---------------------------------------------------------------------------
 * Optional window: PS3RECOMP_VK_WINDOW=1
 *
 * An SDL2 window with a Vulkan swapchain. Rendering still targets the
 * offscreen image, so readback and every test are unchanged; a present then
 * also blits that image into the next swapchain image. Any failure on this
 * path drops back to headless with a message instead of failing the run.
 * -------------------------------------------------------------------------*/
static int vk_env_on(const char* name)
{
    const char* v = getenv(name);
    return v && v[0] && v[0] != '0';
}

static int vk_select_display(void)
{
    const int count=SDL_GetNumVideoDisplays();
    const char* value=getenv("PS3RECOMP_VK_DISPLAY");
    if (!value || !value[0]) return 0;
    int index=0;
    if (rsx_display_parse_index(value,count,&index)) {
        VK_LOG("PS3RECOMP_VK_DISPLAY=%s invalid (display count %d); using display 0\n",value,count);
        return 0;
    }
    return index;
}

static int vk_selected_display_bounds(SDL_Rect* bounds)
{
    if (SDL_GetDisplayUsableBounds(s_vk.selected_display,bounds)==0 &&
        bounds->w>0 && bounds->h>0) return 0;
    if (SDL_GetDisplayBounds(s_vk.selected_display,bounds)==0 &&
        bounds->w>0 && bounds->h>0) return 0;
    VK_LOG("display %d bounds unavailable: %s\n",s_vk.selected_display,SDL_GetError());
    return -1;
}

/* The RSX canvas remains at its requested size; only the SDL client area is
 * shrunk. Leave room for decorations and the taskbar on the chosen display. */
static void vk_place_window_on_display(void)
{
    SDL_Rect bounds;
    if (!s_vk.window || vk_selected_display_bounds(&bounds)) return;
    int top=0,left=0,bottom=0,right=0;
    if (SDL_GetWindowBordersSize(s_vk.window,&top,&left,&bottom,&right)!=0)
        top=left=bottom=right=0;
    if (top<0) top=0; if (left<0) left=0;
    if (bottom<0) bottom=0; if (right<0) right=0;
    const int reserve_w=(left+right)>64 ? left+right : 64;
    const int reserve_h=(top+bottom)>96 ? top+bottom : 96;
    const uint32_t available_w=(uint32_t)(bounds.w>reserve_w ? bounds.w-reserve_w : 1);
    const uint32_t available_h=(uint32_t)(bounds.h>reserve_h ? bounds.h-reserve_h : 1);
    int current_w=0,current_h=0;
    SDL_GetWindowSize(s_vk.window,&current_w,&current_h);
    uint32_t w=1,h=1;
    if (rsx_display_window_size((uint32_t)(current_w>0?current_w:1),
                                (uint32_t)(current_h>0?current_h:1),
                                available_w,available_h,&w,&h)==0 &&
        ((int)w!=current_w || (int)h!=current_h))
        SDL_SetWindowSize(s_vk.window,(int)w,(int)h);
    SDL_GetWindowSize(s_vk.window,&current_w,&current_h);
    const int outer_w=current_w+left+right,outer_h=current_h+top+bottom;
    SDL_SetWindowPosition(s_vk.window,
                          bounds.x+left+(bounds.w-outer_w)/2,
                          bounds.y+top+(bounds.h-outer_h)/2);
    const char* display_name=SDL_GetDisplayName(s_vk.selected_display);
    VK_LOG("window display %d (%s): usable %dx%d, client %dx%d, canvas %ux%u\n",
           s_vk.selected_display,display_name?display_name:"unknown",
           bounds.w,bounds.h,current_w,current_h,s_vk.width,s_vk.height);
}

static void vk_window_drop(const char* why)
{
    if (why) VK_LOG("window disabled, continuing headless: %s\n", why);
    if (s_vk.device && pvkDeviceWaitIdle) {
        pvkDeviceWaitIdle(s_vk.device);
        for (u32 i = 0; i < 8; i++)
            if (s_vk.sc_done[i]) {
                pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
                s_vk.sc_done[i] = VK_NULL_HANDLE;
            }
        if (s_vk.sem_acquire) pvkDestroySemaphore(s_vk.device, s_vk.sem_acquire, NULL);
        if (s_vk.swapchain && pvkDestroySwapchainKHR)
            pvkDestroySwapchainKHR(s_vk.device, s_vk.swapchain, NULL);
    }
    if (s_vk.surface && s_vk.instance && pvkDestroySurfaceKHR)
        pvkDestroySurfaceKHR(s_vk.instance, s_vk.surface, NULL);
    if (s_vk.window) {
        ps3_pad_keyboard_detach();
        SDL_DestroyWindow(s_vk.window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    s_vk.window = NULL;
    s_vk.surface = VK_NULL_HANDLE;
    s_vk.swapchain = VK_NULL_HANDLE;
    s_vk.sem_acquire = VK_NULL_HANDLE;
    s_vk.sc_count = 0;
    s_vk.windowed = 0;
    s_vk.swapchain_dirty = 0;
    s_vk.present_deferred = 0;
}

static void vk_window_open(const char* title)
{
    if (!vk_env_on("PS3RECOMP_VK_WINDOW")) return;
    /* Windows display scaling: a DPI-unaware process gets a window Windows
     * enlarges (1280x720 becomes 1600x900 at 125%) while its Vulkan surface
     * reports the unscaled size, so the image filled only the top-left of the
     * window. Per-monitor DPI awareness with SDL's DPI scaling keeps the
     * window's apparent size and gives the swapchain the real pixel size,
     * which the present blit then fills. Read by SDL when video starts. */
    if (SDL_WasInit(SDL_INIT_VIDEO))
        VK_LOG("window: SDL video already started; DPI awareness hint not applied\n");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        VK_LOG("window disabled, continuing headless: SDL video init failed: %s\n", SDL_GetError());
        return;
    }
#if !defined(_WIN32) || defined(PS3RECOMP_PAD_USE_SDL2)
    /* SDL2-compat delegates to SDL3, whose subsystem initialization belongs
     * to the video thread. Do this before event pumping/guest execution can
     * race cellPadInit. The existing input backend borrows this process-wide
     * controller reference; WasInit avoids adding refs when windows reopen. */
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER) &&
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        VK_LOG("window disabled: SDL controller init failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return;
    }
#endif
#if !defined(_WIN32) || defined(PS3RECOMP_AUDIO_USE_SDL2)
    /* cellAudio also borrows its SDL subsystem reference for process life.
     * Keep its initialization out of the guest thread for the same reason. */
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        VK_LOG("window disabled: SDL audio init failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return;
    }
#endif
    s_vk.selected_display=vk_select_display();
    SDL_Rect bounds;
    uint32_t window_w=s_vk.width,window_h=s_vk.height;
    if (vk_selected_display_bounds(&bounds)) {
        bounds.x=bounds.y=0;
        bounds.w=(int)s_vk.width;bounds.h=(int)s_vk.height;
    }
    const uint32_t available_w=(uint32_t)(bounds.w>64 ? bounds.w-64 : 1);
    const uint32_t available_h=(uint32_t)(bounds.h>96 ? bounds.h-96 : 1);
    (void)rsx_display_window_size(s_vk.width,s_vk.height,available_w,available_h,
                                  &window_w,&window_h);
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    s_vk.window = SDL_CreateWindow(title && title[0] ? title : "ps3recomp",
                                   SDL_WINDOWPOS_CENTERED_DISPLAY(s_vk.selected_display),
                                   SDL_WINDOWPOS_CENTERED_DISPLAY(s_vk.selected_display),
                                   (int)window_w, (int)window_h, flags);
    s_vk.windowed = 1;          /* so vk_window_drop also shuts SDL video down */
    if (!s_vk.window) {
        char why[256];
        snprintf(why, sizeof why, "SDL_CreateWindow failed: %s", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    vk_place_window_on_display();
    if (vk_env_on("PS3RECOMP_VK_FULLSCREEN") &&
        SDL_SetWindowFullscreen(s_vk.window,SDL_WINDOW_FULLSCREEN_DESKTOP)!=0)
        VK_LOG("startup fullscreen transition failed: %s\n",SDL_GetError());
    ps3_pad_keyboard_attach(SDL_GetWindowID(s_vk.window));
    ps3_pad_keyboard_focus(SDL_GetWindowID(s_vk.window),
                           !!(SDL_GetWindowFlags(s_vk.window)&SDL_WINDOW_INPUT_FOCUS));
    unsigned n = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, NULL) || n == 0 || n > 16 ||
        !SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, s_vk.inst_exts)) {
        char why[256];
        snprintf(why, sizeof why, "no Vulkan surface extensions (%s)", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    s_vk.inst_ext_count = n;
    const char* hold = getenv("PS3RECOMP_VK_HOLD");
    s_vk.hold_seconds = hold ? atof(hold) : 0.0;
    VK_LOG("window: SDL video driver '%s', %u surface extension(s)\n",
           SDL_GetCurrentVideoDriver(), n);
}

static int vk_create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_vk.phys, s_vk.surface, &caps),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    /* Zero extent is a temporary window state, not a lost device. Preserve
     * the old swapchain and retry after the event loop observes restoration.
     * Check before clamping: a minimum extent must not hide a zero drawable. */
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, hh = 0;
        SDL_Vulkan_GetDrawableSize(s_vk.window, &w, &hh);
        if (w <= 0 || hh <= 0) return 1;
        ext.width = (u32)w; ext.height = (u32)hh;
        if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    if (!ext.width || !ext.height) return 1;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        VK_LOG("swapchain images cannot be blit destinations\n"); return -1;
    }

    u32 nf = 0;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, NULL),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (nf == 0) { VK_LOG("surface reports no formats\n"); return -1; }
    if (nf > 64) nf = 64;
    VkSurfaceFormatKHR fmts[64];
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, fmts),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    /* UNORM, not SRGB: the offscreen target holds the guest's values as-is. */
    VkSurfaceFormatKHR fmt = fmts[0];
    if (nf == 1 && fmts[0].format == VK_FORMAT_UNDEFINED) {
        fmt.format = VK_FORMAT_B8G8R8A8_UNORM;
    } else {
        static const VkFormat pref[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
        int found = 0;
        for (size_t k = 0; k < 2 && !found; k++)
            for (u32 i = 0; i < nf; i++)
                if (fmts[i].format == pref[k]) { fmt = fmts[i]; found = 1; break; }
    }
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, fmt.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        VK_LOG("swapchain format %d cannot be a blit destination\n", (int)fmt.format); return -1;
    }

    u32 count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    static const VkCompositeAlphaFlagBitsKHR alphas[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (size_t k = 0; k < 4; k++)
        if (caps.supportedCompositeAlpha & alphas[k]) { alpha = alphas[k]; break; }

    VkSwapchainKHR old = s_vk.swapchain;
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s_vk.surface, .minImageCount = count,
        .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
        .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = alpha,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,   /* the one mode every driver has */
        .clipped = VK_TRUE, .oldSwapchain = old,
    };
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = pvkCreateSwapchainKHR(s_vk.device, &sci, NULL, &sc);
    if (old) pvkDestroySwapchainKHR(s_vk.device, old, NULL);
    s_vk.swapchain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) { VK_LOG("vkCreateSwapchainKHR failed (VkResult %d)\n", (int)r); return -1; }
    s_vk.swapchain = sc;

    u32 n = 0;
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, NULL), "vkGetSwapchainImagesKHR");
    if (n == 0 || n > 8) { VK_LOG("unsupported swapchain image count %u\n", n); return -1; }
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, s_vk.sc_images), "vkGetSwapchainImagesKHR");

    for (u32 i = 0; i < 8; i++)
        if (s_vk.sc_done[i]) {
            pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
            s_vk.sc_done[i] = VK_NULL_HANDLE;
        }
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (u32 i = 0; i < n; i++)
        VK_CHECK(pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sc_done[i]), "vkCreateSemaphore");

    s_vk.sc_extent = ext;
    s_vk.sc_count = n;
    VK_LOG("window swapchain %ux%u, %u images, format %d\n", ext.width, ext.height, n, (int)fmt.format);
    return 0;
}

static void vk_swapchain_lost(void)
{
    s_vk.swapchain_dirty = 1;
    s_vk.present_deferred = 1;
    if (pvkDeviceWaitIdle(s_vk.device) != VK_SUCCESS) {
        vk_window_drop("device wait failed during swapchain recreation"); return;
    }
    int result = vk_create_swapchain();
    if (result < 0) vk_window_drop("the swapchain could not be recreated");
    else if (!result) s_vk.swapchain_dirty = 0;
}

/* Do not consume another engine frame while a prior guest flip is waiting for
 * the host window. The caller keeps pumping events and retains that flip. */
static int vk_prepare_window_present(void)
{
    s_vk.present_deferred = 0;
    if (!s_vk.windowed) return 1;
    if (SDL_GetWindowFlags(s_vk.window) & SDL_WINDOW_MINIMIZED) {
        s_vk.swapchain_dirty = 1;
        s_vk.present_deferred = 1;
        return 0;
    }
    if (s_vk.swapchain_dirty || !s_vk.swapchain) {
        vk_swapchain_lost();
        if (!s_vk.windowed || s_vk.swapchain_dirty) return 0;
        s_vk.present_deferred = 0;
    }
    return 1;
}

/* The largest rectangle with the source's aspect ratio that fits the
 * destination, centred. Equal shapes give the whole destination. */
static void vk_fit_rect(u32 sw, u32 sh, u32 dw, u32 dh, u32* x, u32* y, u32* w, u32* h)
{
    rsx_display_rect r={0,0,dw,dh};
    (void)rsx_display_fit(sw,sh,dw,dh,&r);
    *x=r.x; *y=r.y; *w=r.w; *h=r.h;
}

/* Blit the offscreen target into the next swapchain image and present it. */
static void vk_present_window(void)
{
    vk_pcache_tick(s_vk.device);
    if (!s_vk.windowed || !s_vk.swapchain) return;

    VkImage display = s_vk.display_image ? s_vk.display_image : s_vk.color.img;
    u32 idx = 0;
    VkResult r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, UINT64_MAX,
                                        s_vk.sem_acquire, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_LOG("vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r); return;
    }

    /* This submission bypasses vk_submit_and_wait, so nothing may be pending. */
    vk_flush_pending(0);
    if (vk_begin()) return;
    vk_barrier_image(display, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s_vk.sc_images[idx], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    /* Keep the image's aspect ratio: a 16:9 frame in a 16:10 fullscreen
     * window (Steam Deck 1280x800) or an ultrawide desktop is centred with
     * black bars instead of being stretched to fill it. */
    const u32 dw = s_vk.sc_extent.width, dh = s_vk.sc_extent.height;
    u32 fx, fy, fit_w, fit_h;
    /* Crop only the bars we added, then fit once. Fitting the whole canvas
     * again leaves bars on all four sides when the window aspect changes. */
    const rsx_display_rect content=s_vk.content_rect;
    vk_fit_rect(content.w, content.h, dw, dh, &fx, &fy, &fit_w, &fit_h);
    const int32_t fit_x = (int32_t)fx, fit_y = (int32_t)fy;
    if (fit_w != dw || fit_h != dh) {
        const VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
        const VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        pvkCmdClearColorImage(s_vk.cmd, s_vk.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              &black, 1, &all);
        VkImageMemoryBarrier cleared = b;
        cleared.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        cleared.oldLayout = cleared.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 0, NULL, 1, &cleared);
    }
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { (int32_t)content.x, (int32_t)content.y, 0 },
                        { (int32_t)(content.x+content.w), (int32_t)(content.y+content.h), 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { fit_x, fit_y, 0 },
                        { fit_x + (int32_t)fit_w, fit_y + (int32_t)fit_h, 1 } },
    };
    pvkCmdBlitImage(s_vk.cmd, display, VK_IMAGE_LAYOUT_GENERAL,
                    s_vk.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit, VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    if (pvkEndCommandBuffer(s_vk.cmd) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer failed\n"); return; }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sem_acquire,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &s_vk.sc_done[idx],
    };
    pvkResetFences(s_vk.device, 1, &s_vk.fence);
    if (pvkQueueSubmit(s_vk.queue, 1, &si, s_vk.fence) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (window) failed\n"); return;
    }
    if (pvkWaitForFences(s_vk.device, 1, &s_vk.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        VK_LOG("vkWaitForFences (window) failed\n"); return;
    }

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
        .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
    };
    r = pvkQueuePresentKHR(s_vk.queue, &pi);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        s_vk.successful_window_presents++;
        if (s_vk.successful_window_presents <= 4)
            VK_LOG("window present %llu accepted (VkResult %d)\n",
                   (unsigned long long)s_vk.successful_window_presents, (int)r);
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
    else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
}

static void vk_dump_ppm(const unsigned char* rgba)
{
    if(rsx_dump_ppm(s_vk.dump_path,rgba,s_vk.width,s_vk.height))
        VK_LOG("cannot publish complete frame dump %s\n",s_vk.dump_path);
}

static void vk_cb_present(void* ud, u32 buffer_id)
{ vk_flush_pending(0);
    (void)ud; (void)buffer_id;
    if (!s_vk.device) return;

    VkImage display = s_vk.color.img;
    if (s_vk.fxaa_enabled) {
        if (vk_fxaa_apply(&display)) {
            ps3_strict_boot_guard("vulkan-fxaa", "requested FXAA pass failed");
            return;
        }
        if (++s_vk.fxaa_frames <= 4)
            VK_LOG("FXAA frame %llu filtered at %ux%u\n", (unsigned long long)s_vk.fxaa_frames, s_vk.width, s_vk.height);
    }
    /* Sharpen at display resolution unless FSR1's own RCAS already ran. */
    const float sharpen = vk_sharpen_stops();
    if (sharpen >= 0.0f && !s_vk.fsr1_sharpened && !s_vk.rcas_failed) {
        const vk_image* src = s_vk.fxaa_enabled ? &s_fxaa.out : &s_vk.color;
        VkImage sharpened = VK_NULL_HANDLE;
        if (vk_rcas_apply(src, s_vk.width, s_vk.height, sharpen, &sharpened)) {
            s_vk.rcas_failed = 1;
            VK_LOG("RCAS sharpening unavailable; presenting unsharpened\n");
        } else {
            display = sharpened;
            if (++s_vk.rcas_frames <= 2)
                VK_LOG("RCAS frame %llu sharpened at %ux%u\n", (unsigned long long)s_vk.rcas_frames, s_vk.width, s_vk.height);
        }
    }
    s_vk.fsr1_sharpened = 0;
    s_vk.display_image = display;
    /* The whole frame comes back to host memory for the frame dump, the
     * vtable path's centre-pixel probe and the offscreen GPU suites. A
     * windowed draw-engine session needs it only on dump frames (the engine
     * keeps its own probe), so it skips a 3.7 MB copy and a GPU wait. */
    const int dump_due = s_vk.dump_path && ((s_vk.presented + 1u) % s_vk.dump_every) == 0;
    if (s_vk.windowed && s_vk.eng_active && !dump_due) {
        s_vk.presented++;
        s_g.last_draws = s_g.draws;
        s_g.draws = 0;
        vk_present_window();
        return;
    }
    if (vk_begin()) return;
    vk_barrier_image(display, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { s_vk.width, s_vk.height, 1 },
    };
    pvkCmdCopyImageToBuffer(s_vk.cmd, display, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.readback.buf, 1, &region);
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_vk.readback.buf, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &host, 0, NULL);
    if (vk_submit_and_wait()) return;

    vk_invalidate(&s_vk.readback);
    const unsigned char* px = (const unsigned char*)s_vk.readback.ptr;
    size_t c = ((size_t)(s_vk.height / 2) * s_vk.width + s_vk.width / 2) * 4;
    s_vk.last_center = ((u32)px[c] << 16) | ((u32)px[c + 1] << 8) | (u32)px[c + 2];
    if (!s_vk.windowed) s_vk.successful_offscreen_frames++;
    s_vk.presented++;
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
    if (s_vk.dump_path && (s_vk.presented % s_vk.dump_every) == 0) vk_dump_ppm(px);
    vk_present_window();
}

/* ---------------------------------------------------------------------------
 * Register-file draw engine backend (rsx_draw_engine.h)
 *
 * The path the Metal backend takes: the shared engine walks the register
 * file, owns surfaces, texture caching, vertex compaction and pipeline keys,
 * and drives this backend through rsx_draw_backend. Selected with
 * PS3RECOMP_RSX_ENGINE=dispatch; only one of the two paths is registered at a
 * time, since the FIFO walker feeds both and would record every draw twice.
 *
 * Every operation is still one synchronous submission, so submit_and_wait has
 * nothing left to do by the time the engine calls it -- which is exactly the
 * contract it asks for (staging is free to reuse on return).
 *
 * Stage E1: colour and depth targets, clears, present and readback.
 * Pipelines, draws and textures are the next stages; until then
 * pipeline_create reports 0, which the engine caches as "cannot build".
 * -------------------------------------------------------------------------*/
#define VK_ENG_MAX_OBJ 2048          /* the engine's texture cache is 1024 */

enum { VK_ENG_FREE = 0, VK_ENG_COLOR, VK_ENG_DEPTH, VK_ENG_TEXTURE, VK_ENG_VIEW };

typedef struct vk_eng_obj {
    u8       kind;
    VkFormat fmt;
    u32      bpp;                    /* bytes per texel, 0 for depth     */
    u32      w, h;                 /* physical image extent */
    u32      scale, grid_x, grid_y; /* guest raster scale and expanded sample grid */
    u32      guest_w, guest_h;       /* explicit extent for fractional world scale */
    u32      samples, expanded;
    int      expansion_valid;
    vk_image im;                     /* a VIEW owns only im.view            */
    u32      block;                  /* bytes per 4x4 block, 0 = not BC     */
    u32      surface, remap, rsx_fmt;/* VIEW: what it is a view of          */
    u8       cube;                   /* TEXTURE: six layers, a CUBE view    */
    u8       upload_failed;          /* TEXTURE: a mip upload could not stage */
    /* Multisample target: what its expansion pass reuses (rsx_vulkan_msaa.inc). */
    VkImageView     exp_view;
    VkFramebuffer   exp_fb;
    VkDescriptorSet exp_set;
    u32             exp_dst;
    /* DEPTH: its GPU-side A8R8G8B8 texture, or 0. That TEXTURE: the R32_UINT
     * view, framebuffer and source set its pass reuses, and the expanded
     * depth the set names (rsx_vulkan_depth_argb.inc). */
    u32             argb;
    VkImageView     argb_view;
    VkFramebuffer   argb_fb;
    VkDescriptorSet argb_set;
    u32             argb_src;
    u8              dlss_world;      /* COLOR/DEPTH: the world target at DLSS input extent */
} vk_eng_obj;

static vk_eng_obj s_eobj[VK_ENG_MAX_OBJ];

/* Optional live scene collection, implemented after the scene-batch module. */
static int vk_rt_shader_contract(const char* source,u32* mask,u32* bytes);
static int vk_rt_source_sha256(const char* source,char hex[65]);
static void vk_rt_capture_clear(u32 depth,float value);
static void vk_rt_capture_resource(u32 resource);
static void vk_rt_capture_pipeline(u32 pipeline);
static void vk_rt_capture_present(void);
static void vk_rt_capture_shutdown(void);
static void vk_rt_ao_shutdown(void);
static u32 vk_eng_ray_world(void* user,u32 color,u32 depth,u32 logical_w,u32 logical_h,const float projection[16],u32 remap,u32 rsx_format,float radius,float strength);
static u64 s_temporal_geometry,s_temporal_slots;
static void vk_eng_temporal_identity(void* user,u64 geometry,u64 slots){(void)user;s_temporal_geometry=geometry;s_temporal_slots=slots;}
static void vk_dlss_jitter(float out[2]);
static void vk_dlss_jitter_upload(void *constants,size_t bytes);
static int vk_dlss_world_extent(u32 *w,u32 *h);
static void vk_rt_capture_draw(u32 pipeline,rsx_topology topology,const void* vertices,
    u32 vertex_count,u32 stride,const u32* indices,u32 index_count);
/* Defined with the E2 pipeline state further down. */
static void vk_eng_drop_framebuffers(void);
static void vk_eng2_shutdown(void* user);

static struct {
    u32 rt[RSX_BE_MAX_COLOR_TARGETS], nrt, depth;
} s_eng_bound;

static VkFormat vk_eng_format(rsx_be_format f, u32* bpp)
{
    switch (f) {
    case RSX_BE_FMT_R8:            *bpp = 1;  return VK_FORMAT_R8_UNORM;
    case RSX_BE_FMT_R8G8:          *bpp = 2;  return VK_FORMAT_R8G8_UNORM;
    case RSX_BE_FMT_R8G8B8A8:      *bpp = 4;  return VK_FORMAT_R8G8B8A8_UNORM;
    case RSX_BE_FMT_R16:           *bpp = 2;  return VK_FORMAT_R16_UNORM;
    case RSX_BE_FMT_R16G16:        *bpp = 4;  return VK_FORMAT_R16G16_UNORM;
    case RSX_BE_FMT_R16G16F:       *bpp = 4;  return VK_FORMAT_R16G16_SFLOAT;
    case RSX_BE_FMT_R16G16B16A16F: *bpp = 8;  return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RSX_BE_FMT_R32F:          *bpp = 4;  return VK_FORMAT_R32_SFLOAT;
    case RSX_BE_FMT_R32G32B32A32F: *bpp = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RSX_BE_FMT_BC1:           *bpp = 8;  return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  /* per block */
    case RSX_BE_FMT_BC2:           *bpp = 16; return VK_FORMAT_BC2_UNORM_BLOCK;
    case RSX_BE_FMT_BC3:           *bpp = 16; return VK_FORMAT_BC3_UNORM_BLOCK;
    default:                       *bpp = 0;  return VK_FORMAT_UNDEFINED;
    }
}

/* Colour formats the E2 passes render into: RGBA8 for ordinary and display
 * targets, RGBA16F for SET_SURFACE_FORMAT 0xB HDR targets. Both are required
 * colour attachment formats with blending in Vulkan 1.0. */
#define VK_ENG_RT_FORMATS 2
static int vk_eng_rt_index(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:      return 0;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 1;
    default:                            return -1;
    }
}

/* The largest image side the engine creates: the device's own limit once a
 * device is picked, the 4096 Vulkan guarantees before. */
static u32 vk_max_dim(void) { return s_vk.max_dim ? s_vk.max_dim : 4096u; }

static u32 vk_eng_alloc(void)
{
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
        if (s_eobj[i].kind == VK_ENG_FREE) return i;
    VK_LOG("engine: object table full (%d)\n", VK_ENG_MAX_OBJ);
    return 0;
}

static vk_eng_obj* vk_eng_get(u32 handle, int kind)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return NULL;
    vk_eng_obj* o = &s_eobj[handle];
    return (o->kind == kind) ? o : NULL;
}

/* Copy `rows` rows of `w` texels from host memory into a w x h rectangle at
 * (x, y) of one level / layer of an image that is in GENERAL. */
/* Room for `bytes` in the upload ring, flushing (or growing it) when short.
 * Returns 0 and the offset, or -1 to fall back to a waited upload. */
static int vk_upload_ring_reserve(VkDeviceSize bytes, VkDeviceSize* off)
{
    if (!s_batch.defer || !bytes || bytes > VK_UPLOAD_RING_MAX) return -1;
    VkDeviceSize at = (s_batch.rc + VK_UPLOAD_ALIGN - 1u) & ~(VkDeviceSize)(VK_UPLOAD_ALIGN - 1u);
    if (s_batch.ring.buf && at + bytes <= s_batch.ring.size) { *off = at; s_batch.rc = at + bytes; return 0; }
    vk_flush_pending(2);           /* the ring's bytes are free once nothing is pending */
    if (!s_batch.ring.buf || bytes > s_batch.ring.size) {
        VkDeviceSize size = s_batch.ring.size ? s_batch.ring.size : VK_UPLOAD_RING_MIN;
        while (size < bytes) size *= 2u;
        vk_destroy_buffer(&s_batch.ring);
        if (vk_create_host_buffer(&s_batch.ring, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
            !s_batch.ring.coherent) {
            vk_destroy_buffer(&s_batch.ring);
            VK_LOG("texture upload ring (%llu bytes) unavailable; uploads waited\n", (unsigned long long)size);
            s_batch.defer = 0;
            return -1;
        }
    }
    s_batch.rc = bytes; *off = 0;
    return 0;
}

static int vk_eng_upload_region(VkImage img, u32 layer, u32 mip, u32 x, u32 y,
                                u32 w, u32 h, size_t tight, u32 rows,
                                const void* src, u32 row_bytes)
{
    if (tight > row_bytes) tight = row_bytes;
    vk_batch_configure();
    VkDeviceSize off;
    if (vk_upload_ring_reserve((VkDeviceSize)tight * rows, &off) == 0) {
        for (u32 y = 0; y < rows; y++)
            memcpy((u8*)s_batch.ring.ptr + off + (size_t)y * tight, (const u8*)src + (size_t)y * row_bytes, tight);
        if (vk_begin()) return -1;
        vk_barrier_image(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .bufferOffset = off,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 },
            .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyBufferToImage(s_vk.cmd, s_batch.ring.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        s_batch.deferred++;
        s_batch.deferred_uploads++;
        return 0;
    }
    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)tight * rows, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !st.coherent) { vk_destroy_buffer(&st); return -1; }
    for (u32 y = 0; y < rows; y++)
        memcpy((u8*)st.ptr + (size_t)y * tight, (const u8*)src + (size_t)y * row_bytes, tight);
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 },
            .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyBufferToImage(s_vk.cmd, st.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&st);
    return rc;
}

/* The same into the whole of a level, from its origin. */
static int vk_eng_upload_rows(VkImage img, u32 layer, u32 mip, u32 w, u32 h,
                              size_t tight, u32 rows, const void* src, u32 row_bytes)
{
    return vk_eng_upload_region(img, layer, mip, 0, 0, w, h, tight, rows, src, row_bytes);
}


/* NV4097 TEXTURE_CONTROL1 crossbar as a Vulkan component mapping: the shared
 * rsx_texture_component_remap() gives, in A,R,G,B order, which uploaded
 * channel (0..3 = R,G,B,A) or constant each output takes -- the table the
 * Metal backend's swizzle_sel reads. */
static VkComponentSwizzle vk_remap_sel(u8 sel)
{
    switch (sel) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case RSX_REMAP_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    default: return VK_COMPONENT_SWIZZLE_ZERO;
    }
}
static VkComponentMapping vk_remap(u32 remap, u32 rsx_fmt)
{
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    VkComponentMapping m = { vk_remap_sel(sel[1]), vk_remap_sel(sel[2]),
                             vk_remap_sel(sel[3]), vk_remap_sel(sel[0]) };
    return m;
}
static int vk_eng_make_view(VkImage img, VkFormat fmt, u32 levels, VkComponentMapping map,
                            int cube, VkImageView* out)
{
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt, .components = map,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels ? levels : 1, 0, cube ? 6u : 1u },
    };
    return pvkCreateImageView(s_vk.device, &vci, NULL, out) == VK_SUCCESS ? 0 : -1;
}


#include "rsx_vulkan_resolution.inc"

static int vk_eng_init(void* user, u32 width, u32 height)
{ vk_flush_pending(0);
    (void)user; (void)width; (void)height;
    memset(s_eobj, 0, sizeof s_eobj);
    memset(&s_eng_bound, 0, sizeof s_eng_bound);
    return s_vk.device ? vk_eng_resolution_config() : -1;
}

static u32 vk_eng_expand_samples(u32 surface);
static void vk_eng_expansion_shutdown(void);
static void vk_eng_expansion_forget(vk_eng_obj* src);
static int vk_eng_pack_samples(u32 surface, u32 expanded);
static void vk_eng_pack_shutdown(void);
static void vk_eng_depth_argb_forget(vk_eng_obj* o);
static void vk_eng_depth_argb_shutdown(void);
#ifdef PS3RECOMP_RSX_CACAO
static void vk_eng_cacao_shutdown(void);
#endif

static void vk_eng_release(void* user, u32 handle)
{   (void)user;
    vk_async_drain();
    /* A plain texture pending commands may still sample goes to the
     * graveyard, destroyed once the next submission has completed; anything
     * else (targets, their views and framebuffers) waits as before. */
    if (handle && handle < VK_ENG_MAX_OBJ && s_eobj[handle].kind == VK_ENG_TEXTURE &&
        !s_eobj[handle].expanded && !s_eobj[handle].exp_fb && !s_eobj[handle].argb_fb && s_batch.defer &&
        s_batch.recording && (s_batch.pending || s_batch.deferred)) {
        if (s_grave.n == VK_GRAVE_MAX) vk_flush_pending(1);
        if (s_batch.recording && (s_batch.pending || s_batch.deferred)) {
            s_grave.im[s_grave.n++] = s_eobj[handle].im;
            memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
            return;
        }
    }
    vk_flush_pending(0);
    if (!handle || handle >= VK_ENG_MAX_OBJ || s_eobj[handle].kind == VK_ENG_FREE) return;
    if(s_vk.rt_enabled)vk_rt_capture_resource(handle);
    if (s_eobj[handle].kind == VK_ENG_COLOR)
        for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
            if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == handle) {
                if(s_vk.rt_enabled)vk_rt_capture_resource(i);
                vk_destroy_image(&s_eobj[i].im);
                memset(&s_eobj[i], 0, sizeof s_eobj[i]);
            }
    if (s_eobj[handle].kind == VK_ENG_DEPTH && s_eobj[handle].surface) {
        const u32 snap = s_eobj[handle].surface;
        s_eobj[handle].surface = 0;
        if (vk_eng_get(snap, VK_ENG_TEXTURE)) vk_eng_release(user, snap);
    }
    if (s_eobj[handle].kind == VK_ENG_DEPTH && s_eobj[handle].argb) {
        const u32 argb = s_eobj[handle].argb;
        s_eobj[handle].argb = 0;
        if (vk_eng_get(argb, VK_ENG_TEXTURE)) vk_eng_release(user, argb);
    }
    if (s_eobj[handle].kind == VK_ENG_COLOR || s_eobj[handle].kind == VK_ENG_DEPTH)
        vk_eng_drop_framebuffers();
    vk_eng_expansion_forget(&s_eobj[handle]);
    vk_eng_depth_argb_forget(&s_eobj[handle]);
    if (s_eobj[handle].expanded) {
        const u32 expanded = s_eobj[handle].expanded;
        s_eobj[handle].expanded = 0;
        vk_eng_release(user, expanded);
    }
    vk_destroy_image(&s_eobj[handle].im);      /* idle: submissions are waited on */
    memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
}

static void vk_eng_shutdown(void* user)
{ vk_flush_pending(0);
#ifdef PS3RECOMP_RSX_CACAO
    vk_eng_cacao_shutdown();
#endif
    vk_eng2_shutdown(user);
    vk_eng_expansion_shutdown();
    vk_eng_pack_shutdown();
    vk_eng_depth_argb_shutdown();
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++) vk_eng_release(user, i);
}

static void vk_eng_submit_and_wait(void* user, u32 reason)
{ vk_flush_pending(0);
    (void)user; (void)reason;      /* batched draws were just submitted and waited on */
}

static u32 vk_eng_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                      const void* seed, u32 seed_row_bytes)
{ vk_flush_pending(0);
    (void)user;
    const u32 guest_w=w,guest_h=h;
    if (vk_eng_scale_extent(&w,&h,1)) return 0;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    /* Only formats the E2 passes are built for (vk_eng_rt_index). */
    if (vk_eng_rt_index(vf) < 0 || !w || !h || w > vk_max_dim() || h > vk_max_dim()) {
        VK_LOG("engine: colour target format %d %ux%u not supported yet\n", (int)fmt, w, h);
        return 0;
    }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_create_image(&o->im, vf, w, h,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_COLOR; o->fmt = vf; o->bpp = bpp; o->w = w; o->h = h; o->scale=s_render_scale;
    /* UNDEFINED -> GENERAL, then the guest's own bytes when the engine could
     * resolve them, else transparent black so the contents are defined. */
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue zero = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    if (vk_submit_and_wait()) { vk_eng_release(user, hd); return 0; }
    if (seed && seed_row_bytes && vk_eng_scaled_write(hd,o,0,0,guest_w,guest_h,seed,seed_row_bytes)) {
        vk_eng_release(user,hd); return 0;
    }
    return hd;
}

static u32 vk_eng_depth_target_create(void* user, u32 w, u32 h)
{ vk_flush_pending(0);
    (void)user;
    if (vk_eng_scale_extent(&w,&h,1)) return 0;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_create_image(&o->im, s_vk.depth_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        s_vk.depth_aspect)) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_DEPTH; o->fmt = s_vk.depth_format; o->w = w; o->h = h; o->scale=s_render_scale;
    /* Far plane, for the reason the display depth starts there. */
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (vk_submit_and_wait()) { vk_eng_release(user, hd); return 0; }
    return hd;
}

static u32 vk_eng_target_multisample(void* user, rsx_be_format fmt, u32 w, u32 h, u32 samples, int depth)
{
    const u32 guest_w=w,guest_h=h;
    u32 quality_w=0,quality_h=0;
    const int quality=vk_eng_quality_world(w,h,samples,&quality_w,&quality_h);
    if ((samples != 2 && samples != 4) || vk_eng_scale_extent(&w,&h,samples)) return 0;
    if(quality){w=quality_w;h=quality_h;}
    u32 bpp = 0;
    const VkFormat vf = depth ? s_vk.depth_format : vk_eng_format(fmt, &bpp);
    if (!depth && vk_eng_rt_index(vf) < 0) return 0;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    const VkImageAspectFlags aspect = depth ? s_vk.depth_aspect : VK_IMAGE_ASPECT_COLOR_BIT;
    if (vk_create_image_levels_samples(&o->im, vf, w, h, 1,
            (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, aspect, samples)) {
        vk_destroy_image(&o->im); memset(o, 0, sizeof *o); return 0;
    }
    o->kind = depth ? VK_ENG_DEPTH : VK_ENG_COLOR;
    o->fmt = vf; o->bpp = bpp; o->w = w; o->h = h; o->scale=s_render_scale; o->samples = samples;
    if(quality){o->guest_w=guest_w;o->guest_h=guest_h;o->dlss_world=1;}
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkImageSubresourceRange range = {aspect, 0, 1, 0, 1};
    if (depth) {
        VkClearDepthStencilValue far_plane = {1.0f, 0};
        pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &range);
    } else {
        VkClearColorValue zero = { .float32 = {0,0,0,0} };
        pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    if (vk_submit_and_wait()) { vk_eng_release(user, hd); return 0; }
    VK_LOG("engine: native %u-sample %s attachment %ux%u\n", samples, depth ? "depth" : "color", w, h);
    return hd;
}
static u32 vk_eng_color_multisample(void* user, rsx_be_format fmt, u32 w, u32 h, u32 samples)
{ vk_flush_pending(0); return vk_eng_target_multisample(user, fmt, w, h, samples, 0); }
static u32 vk_eng_depth_multisample(void* user, u32 w, u32 h, u32 samples)
{ vk_flush_pending(0); return vk_eng_target_multisample(user, RSX_BE_FMT_R8G8B8A8, w, h, samples, 1); }

static void vk_eng_bind_targets(void* user, const u32* surfaces, u32 count, u32 depth)
{
    (void)user;
    if (count > RSX_BE_MAX_COLOR_TARGETS) count = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        s_eng_bound.rt[i] = (i < count && surfaces) ? surfaces[i] : 0;
    s_eng_bound.nrt = count;
    s_eng_bound.depth = depth;
}

static void vk_eng_clear_color(void* user, u32 surface, const float rgba[4])
{   /* A recorded clear, ordered on the GPU like a draw: it joins the batch. */
    vk_batch_configure();
    if (!s_batch.defer) vk_flush_pending(0);
    (void)user;
    /* The debug hook reports the last colour asked for, as Metal's does. */
    s_vk.clear_argb = ((u32)(rgba[3] * 255.0f + 0.5f) << 24) |
                      ((u32)(rgba[0] * 255.0f + 0.5f) << 16) |
                      ((u32)(rgba[1] * 255.0f + 0.5f) <<  8) |
                       (u32)(rgba[2] * 255.0f + 0.5f);
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || vk_begin()) return;
    o->expansion_valid = 0;
    VkClearColorValue v;
    memcpy(v.float32, rgba, sizeof v.float32);
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    vk_submit_or_defer();
}

static void vk_eng_clear_depth_stencil(void* user, u32 depth, u32 flags,
                                       float depth_value, u8 stencil)
{   vk_batch_configure();
    if (!s_batch.defer) vk_flush_pending(0);
    (void)user;
    vk_eng_obj* o = vk_eng_get(depth, VK_ENG_DEPTH);
    VkImageAspectFlags aspect = 0;
    if (flags & RSX_BE_CLEAR_DEPTH) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (flags & RSX_BE_CLEAR_STENCIL) aspect |= s_vk.depth_aspect & VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!o || !aspect || vk_begin()) return;
    o->expansion_valid = 0;
    if(s_vk.rt_enabled && (flags&RSX_BE_CLEAR_DEPTH))vk_rt_capture_clear(depth,depth_value);
    VkClearDepthStencilValue d = { depth_value, stencil };
    VkImageSubresourceRange r = { aspect, 0, 1, 0, 1 };
    vk_barrier_image(o->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    vk_submit_or_defer();
}

/* The named surface becomes the frame: blit it onto the display image, then
 * the existing present does the readback, the PPM dump and the window. */
static void vk_eng_set_present_aspect(void* user,float aspect)
{
    (void)user;
    s_vk.present_aspect=isfinite(aspect) && aspect>=16.0f/9.0f && aspect<=32.0f/9.0f ? aspect : 0;
}

static void vk_eng_present_impl(void* user, u32 surface);
static void vk_eng_present(void* user, u32 surface)
{
    vk_eng_present_impl(user, surface);
    vk_async_kick();
}
static void vk_eng_present_impl(void* user, u32 surface)
{ vk_flush_pending(0);
    if(s_vk.rt_enabled)vk_rt_capture_present();
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (o && o->samples > 1) o = vk_eng_get(vk_eng_expand_samples(surface), VK_ENG_TEXTURE);
    VkImage display = o ? o->im.img : VK_NULL_HANDLE;
    u32 display_width = o ? o->w : 0, display_height = o ? o->h : 0;
    /* An output size of another shape (ultrawide, 16:10) shows the guest
     * image at its own aspect with black bars: stretching it distorts the
     * picture, and the game renders no wider view. */
    u32 fit_x = 0, fit_y = 0, fit_w = s_vk.width, fit_h = s_vk.height;
    if (o && !(s_vk.present_aspect>0 &&
               fabs((double)s_vk.present_aspect-(double)s_vk.width/s_vk.height)<0.000001)) {
        const u32 aspect_h=s_vk.present_aspect>0 ? 1000000u : o->h;
        const u32 aspect_w=s_vk.present_aspect>0 ? (u32)(s_vk.present_aspect*1000000.0f+0.5f) : o->w;
        vk_fit_rect(aspect_w,aspect_h,s_vk.width,s_vk.height,&fit_x,&fit_y,&fit_w,&fit_h);
    }
    s_vk.content_rect=(rsx_display_rect){fit_x,fit_y,fit_w,fit_h};
    if (o && s_vk.fsr1_enabled && !s_vk.fsr1_failed) {
        if (fit_w < o->w || fit_h < o->h ||
            (o->fmt != VK_FORMAT_R8G8B8A8_UNORM && o->fmt != VK_FORMAT_B8G8R8A8_UNORM)) {
            if (!s_vk.fsr1_scale_fallback)
                VK_LOG("FSR1 bypass: downscale or non-display-UNORM source; using ordinary display blit\n");
            s_vk.fsr1_scale_fallback = 1;
        } else {
            s_vk.fsr1_scale_fallback = 0;
            const float stops = vk_sharpen_stops();
            if (vk_fsr1_apply(&o->im, o->w, o->h, fit_w, fit_h, stops >= 0.0f ? stops : 0.2f, &display)) {
                s_vk.fsr1_failed = 1;
                VK_LOG("FSR1 unavailable: using ordinary display blit for this session\n");
            } else {
                s_vk.fsr1_sharpened = 1;
                display_width = fit_w;
                display_height = fit_h;
                if (++s_vk.fsr1_frames <= 4)
                    VK_LOG("FSR1 EASU+RCAS frame %llu: guest %ux%u to display %ux%u\n",
                        (unsigned long long)s_vk.fsr1_frames, o->w, o->h,
                        display_width, display_height);
            }
        }
    }
    if (o && !vk_begin()) {
        vk_barrier_image(display, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        if (fit_w != s_vk.width || fit_h != s_vk.height) {
            const VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
            const VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &all);
            vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        }
        VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { (int32_t)display_width, (int32_t)display_height, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { (int32_t)fit_x, (int32_t)fit_y, 0 },
                            { (int32_t)(fit_x + fit_w), (int32_t)(fit_y + fit_h), 1 } },
        };
        pvkCmdBlitImage(s_vk.cmd, display, VK_IMAGE_LAYOUT_GENERAL,
                        s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                        (display_width == fit_w && display_height == fit_h) ? VK_FILTER_NEAREST
                                                                           : VK_FILTER_LINEAR);
        if (vk_submit_and_wait()) return;
    }
    vk_cb_present(&s_vk, 0);
}

/* Pixels a 2D transfer put into guest memory, pushed into the colour target
 * that rectangle belongs to. */
static void vk_eng_color_target_write(void* user, u32 surface, u32 x, u32 y,
                                      u32 w, u32 h, const void* rows, u32 row_bytes)
{ vk_flush_pending(0);
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (o && o->samples > 1) {
        const u32 expanded = vk_eng_expand_samples(surface);
        vk_eng_obj* dst = vk_eng_get(expanded, VK_ENG_TEXTURE);
        if (!dst || o->fmt != VK_FORMAT_R8G8B8A8_UNORM || !rows || !w || !h ||
            x > dst->w/(dst->scale?dst->scale:1u) || y > dst->h/(dst->scale?dst->scale:1u) ||
            (size_t)w*o->bpp > row_bytes) {
            ps3_strict_boot_guard("vulkan-msaa-write", "invalid individual-sample upload surface=%u", surface);
            return;
        }
        /* Expand first so a partial transfer retains every untouched sample.
         * Cache invalidation prevents failed uploads from becoming texture data. */
        o->expansion_valid = 0;
        if (vk_eng_scaled_write(expanded,dst,x,y,w,h,rows,row_bytes) ||
            !vk_eng_pack_samples(surface,expanded))
            ps3_strict_boot_guard("vulkan-msaa-write", "individual-sample upload failed surface=%u", surface);
        return;
    }
    if (!o || o->fmt != VK_FORMAT_R8G8B8A8_UNORM) return;   /* rows are R,G,B,A */
    if (vk_eng_scaled_write(surface,o,x,y,w,h,rows,row_bytes))
        ps3_strict_boot_guard("vulkan-scaled-write","color upload failed surface=%u",surface);
}

static int vk_eng_readback_rows_raw(u32 surface, u32 x, u32 y, u32 w, u32 h,
                               void* out, u32 out_pitch)
{
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o) o = vk_eng_get(surface, VK_ENG_TEXTURE);
    if (o && o->samples > 1) o = vk_eng_get(vk_eng_expand_samples(surface), VK_ENG_TEXTURE);
    if (!o || !out || !w || !h || !o->bpp || o->block ||
        x > o->w || w > o->w-x || y > o->h || h > o->h-y ||
        (size_t)w*o->bpp > out_pitch) return -1;
    const size_t tight = (size_t)w * o->bpp;
    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)tight * h, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&st); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        rc = vk_submit_and_wait();
        if (!rc) {
            vk_invalidate(&st);
            for (u32 row = 0; row < h; row++)
                memcpy((u8*)out + (size_t)row * out_pitch, (const u8*)st.ptr + (size_t)row * tight, tight);
        }
    }
    vk_destroy_buffer(&st);
    return rc ? -1 : 0;
}

static int vk_eng_readback_rows(u32 surface,u32 x,u32 y,u32 w,u32 h,void* out,u32 pitch)
{
    vk_eng_obj* o=vk_eng_get(surface,VK_ENG_COLOR);
    if (!o) o=vk_eng_get(surface,VK_ENG_TEXTURE);
    if (o && o->samples>1) { surface=vk_eng_expand_samples(surface);o=vk_eng_get(surface,VK_ENG_TEXTURE); }
    return o ? vk_eng_scaled_read(surface,o,o->bpp,x,y,w,h,out,pitch,vk_eng_readback_rows_raw) : -1;
}

static void vk_eng_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                            void* out, u32 out_pitch)
{ vk_flush_pending(0);
    (void)user;
    if (vk_eng_readback_rows(surface,x,y,w,h,out,out_pitch))
        ps3_strict_boot_guard("vulkan-readback", "color copy failed surface=%u region=%u,%u,%u,%u pitch=%u",
                              surface,x,y,w,h,out_pitch);
}

/* ---- E3: textures ---------------------------------------------------------
 * The engine decodes every level to host rows (rsx_texture_decode) and hands
 * them over one upload each; the crossbar is a property of the view, so it
 * becomes the image view's component mapping. A cube map (faces == 6) is a
 * six-layer cube-compatible image, each face uploaded into its layer, sampled
 * through a CUBE view; the pipelines that sample one use the split set layout
 * (vk_eng_split_layout). */
static u32 vk_eng_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                 u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{   /* A new image and a recorded layout change: nothing here touches what
     * pending draws use, so with deferred uploads it joins their batch. */
    vk_batch_configure();
    if (!s_batch.defer) vk_flush_pending(0);
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    const int bc = (fmt == RSX_BE_FMT_BC1 || fmt == RSX_BE_FMT_BC2 || fmt == RSX_BE_FMT_BC3);
    if (vf == VK_FORMAT_UNDEFINED || !w || !h || w > vk_max_dim() || h > vk_max_dim() ||
        (bc && !s_vk.bc_ok))
        return 0;
    const int cube = (faces == 6);
    if ((faces != 1 && !cube) || (cube && w != h)) return 0;   /* cube faces are square */
    if (!mips) mips = 1;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, .imageType = VK_IMAGE_TYPE_2D,
        .format = vf, .extent = { w, h, 1 }, .mipLevels = mips, .arrayLayers = cube ? 6u : 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements req;
    u32 type;
    if (pvkCreateImage(s_vk.device, &ici, NULL, &o->im.img) != VK_SUCCESS) goto fail;
    pvkGetImageMemoryRequirements(s_vk.device, o->im.img, &req);
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &o->im.mem) != VK_SUCCESS ||
        pvkBindImageMemory(s_vk.device, o->im.img, o->im.mem, 0) != VK_SUCCESS ||
        vk_eng_make_view(o->im.img, vf, mips, vk_remap(remap, rsx_fmt), cube, &o->im.view))
        goto fail;
    o->kind = VK_ENG_TEXTURE; o->fmt = vf; o->w = w; o->h = h; o->cube = (u8)cube;
    o->upload_failed = 0;
    o->bpp = bc ? 0 : bpp; o->block = bc ? bpp : 0;
    /* UNDEFINED -> GENERAL for every level and face before the uploads arrive. */
    if (vk_begin()) goto fail;
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (vk_submit_or_defer()) goto fail;
    return hd;
fail:
    vk_destroy_image(&o->im);
    memset(o, 0, sizeof *o);
    return 0;
}

static void vk_eng_texture_upload(void* user, u32 texture, u32 face, u32 mip, u32 w, u32 h,
                                  const void* src, u32 row_bytes, u32 rows)
{   /* The rows are copied into the ring now and the copy is ordered after
     * the pending draws on the GPU, so it need not wait for them. */
    vk_batch_configure();
    if (!s_batch.defer) vk_flush_pending(0);
    (void)user;
    vk_eng_obj* o = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (!o) return;
    if (!src || !row_bytes || !rows || face >= (o->cube ? 6u : 1u)) {
        o->upload_failed = 1;
        return;
    }
    if(s_vk.rt_enabled)vk_rt_capture_resource(texture);
    /* A BC row is a row of 4x4 blocks; the engine's `rows` already counts them. */
    const size_t tight = o->block ? (size_t)((w + 3u) / 4u) * o->block : (size_t)w * o->bpp;
    if (vk_eng_upload_rows(o->im.img, face, mip, w, h, tight, rows, src, row_bytes))
        o->upload_failed = 1;
}

static int vk_eng_texture_upload_valid(void* user, u32 texture)
{
    (void)user;
    const vk_eng_obj* o = vk_eng_get(texture, VK_ENG_TEXTURE);
    return o && !o->upload_failed;
}

/* A colour target sampled with a unit's crossbar: an extra view of the same
 * image, cached by (surface, crossbar, format) and released with the target. */
static u32 vk_eng_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{   (void)user;
    vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    vk_batch_configure();
    /* A view is a CPU-side lookup or a new VkImageView, and a multisample
     * target's expansion pass is recorded behind the pending draws: nothing
     * here needs the GPU idle. */
    if (!s_batch.defer || !t) vk_flush_pending(0);
    if (!t) return 0;
    if (t->samples > 1) {
        t = vk_eng_get(vk_eng_expand_samples(surface), VK_ENG_TEXTURE);
        if (!t) { ps3_strict_boot_guard("vulkan-msaa-expand", "color sample expansion failed"); return 0; }
    }
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
        if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == surface &&
            s_eobj[i].remap == remap && s_eobj[i].rsx_fmt == rsx_format)
            return i;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_eng_make_view(t->im.img, t->fmt, 1, vk_remap(remap, rsx_format), 0, &o->im.view)) {
        memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_VIEW; o->fmt = t->fmt; o->w = t->w; o->h = t->h;
    o->surface = surface; o->remap = remap; o->rsx_fmt = rsx_format;
    o->scale=t->scale;o->grid_x=t->grid_x;o->grid_y=t->grid_y;o->guest_w=t->guest_w;o->guest_h=t->guest_h;
    return hd;
}
/* ---- E4: depth snapshots --------------------------------------------------
 * A depth target read as a texture, which the engine asks for only after a
 * depth-writing draw. Metal resolves it with a fullscreen pass into R32Float;
 * Vulkan can do it with two copies on the GPU, since a D32_SFLOAT depth
 * aspect copies out as 32-bit floats: depth -> buffer -> an R32_SFLOAT
 * texture. Other depth formats would need a conversion pass: they report 0.
 *
 * Each depth target keeps ONE snapshot texture (its handle in `surface`),
 * refilled on every request, so nothing accumulates however often the engine
 * invalidates and asks again; it goes when the depth target goes. */
static u32 vk_eng_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{ vk_flush_pending(0);
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z) return 0;
    if (z->samples > 1) return vk_eng_expand_samples(depth);
    if (s_vk.depth_format != VK_FORMAT_D32_SFLOAT &&
        s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) {
        static int warned_fmt;
        if (!warned_fmt) { VK_LOG("engine: depth snapshots need D32_SFLOAT, this device uses %d\n",
                                  (int)s_vk.depth_format); warned_fmt = 1; }
        return 0;
    }
    const u32 scale=z->scale?z->scale:1u;
    if (w>UINT32_MAX/scale || h>UINT32_MAX/scale) return 0;
    w*=scale;h*=scale;
    if (w > z->w) w = z->w;
    if (h > z->h) h = z->h;
    if (!w || !h) return 0;

    u32 snap = z->surface;
    vk_eng_obj* t = vk_eng_get(snap, VK_ENG_TEXTURE);
    if (!t || t->w != w || t->h != h) {
        if (t) vk_eng_release(user, snap);
        z->surface = 0;
        snap = vk_eng_alloc();
        if (!snap) return 0;
        t = &s_eobj[snap];
        if (vk_create_image(&t->im, VK_FORMAT_R32_SFLOAT, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0;
        }
        t->kind = VK_ENG_TEXTURE; t->fmt = VK_FORMAT_R32_SFLOAT; t->bpp = 4; t->w = w; t->h = h; t->scale=scale;
        z = vk_eng_get(depth, VK_ENG_DEPTH);          /* same slot; reload for clarity */
        z->surface = snap;
        if (vk_begin()) return 0;
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        if (vk_submit_and_wait()) return 0;
    }

    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)w * h * 4u,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&st); return 0;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(z->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy out = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                                  .imageExtent = { w, h, 1 } };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &out);
        VkBufferMemoryBarrier bb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 1, &bb, 0, NULL);
        VkBufferImageCopy in = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { w, h, 1 } };
        pvkCmdCopyBufferToImage(s_vk.cmd, st.buf, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&st);
    return rc ? 0 : snap;
}
/* The depth aspect of a D32_SFLOAT target copies out as 32-bit floats, which
 * is what the engine asks for; other depth formats report failure. */
static int vk_eng_depth_readback_raw(u32 depth, u32 x, u32 y, u32 w, u32 h,
                                 void* out, u32 out_pitch)
{
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z || !out || !w || !h || x + w > z->w || y + h > z->h ||
        out_pitch < w * 4u || (s_vk.depth_format != VK_FORMAT_D32_SFLOAT &&
                                  s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT))
        return -1;
    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)w * h * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&st); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(z->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        rc = vk_submit_and_wait();
        if (!rc) {
            vk_invalidate(&st);
            for (u32 row = 0; row < h; row++)
                memcpy((u8*)out + (size_t)row * out_pitch,
                       (const u8*)st.ptr + (size_t)row * w * 4u, (size_t)w * 4u);
        }
    }
    vk_destroy_buffer(&st);
    return rc ? -1 : 0;
}
static int vk_eng_depth_readback(void* user,u32 depth,u32 x,u32 y,u32 w,u32 h,float* out,u32 pitch)
{ vk_flush_pending(0);
    (void)user;
    vk_eng_obj* z=vk_eng_get(depth,VK_ENG_DEPTH);
    if (z && z->samples>1) return vk_eng_readback_rows(vk_eng_expand_samples(depth),x,y,w,h,out,pitch);
    return z ? vk_eng_scaled_read(depth,z,4,x,y,w,h,out,pitch,vk_eng_depth_readback_raw) : -1;
}

/* ---- E2: pipelines and draws ---------------------------------------------
 * A pipeline handle carries the two translated modules and the render state
 * the engine keyed it on. Topology is not part of that key (Metal sets it on
 * the encoder), but Vulkan 1.0 bakes it into the pipeline, so each handle
 * builds its VkPipeline per topology on first use.
 *
 * Bindings are the translator's contract (rsx_shader_spirv.h), and the set
 * layout, pipeline layout, descriptor set and constant UBO are the ones the
 * vtable guest path already creates. Vertex inputs are the engine's compact
 * layout: slot n is a float4 at n*16, at location n, as Metal reads it.
 * Known gaps, stated rather than guessed: vertex-texture units (their
 * samplers s0..s3 would alias the pixel samplers' binding), colour targets
 * other than RGBA8 and RGBA16F, depth-only passes, and stencil (the depth format
 * has no stencil aspect yet, so the test is inert). */
#define VK_ENG_MAX_PIPE   512
#define VK_ENG_TOPOLOGIES 5
#define VK_ENG_FB_CACHE   64

typedef struct vk_eng_pipe {
    int                 used;
    VkShaderModule      vs, fs;
    char *scene_vs, *scene_ps;
    char scene_vs_sha256[65],scene_ps_sha256[65];
    int scene_alpha_contract;
    u32 scene_alpha_mask,scene_alpha_fp_bytes;
    rsx_vertex_layout_plan scene_layout;
    rsx_be_render_state rs;
    u32                 nslots, stride, rt_count;
    VkFormat            rt_fmt;          /* picks the render pass it is built against */
    u32                 cube_mask;       /* TextureCube units: the split set layout */
    int                 folklore_composite; /* rsx_folklore_composite.h, at scale > 1 */
    VkPipeline          variant[3][VK_ENG_TOPOLOGIES];
} vk_eng_pipe;

static vk_eng_pipe s_epipe[VK_ENG_MAX_PIPE];

static struct {
    VkRenderPass rp[3][VK_ENG_RT_FORMATS][RSX_BE_MAX_COLOR_TARGETS + 1]; /* by format, colour count */
    struct { u32 key[RSX_BE_MAX_COLOR_TARGETS + 1]; VkFramebuffer fb; } fb[VK_ENG_FB_CACHE];
    u32          fb_count;
    vk_image     scratch_depth;                          /* when no zeta is bound */
    u32          scratch_w, scratch_h, scratch_samples;
    u32          pipeline;
    u32          vs_bytes, ps_bytes;
    u8*          constant_shadow;
    size_t       constant_shadow_size;
    u32          tex[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES];
    u32          tex_mask;
    float        vp[6];
    u32          sc[4];
    int          have_vp, have_sc;
    u32          stencil_ref;
    vk_buffer    indices;
    u32          warned;
    /* The split set layout (vk_eng_split_layout) and its one set, made the
     * first time a pipeline samples a cube map; the 1x1 cube a cube unit
     * with nothing bound samples. */
    VkDescriptorSetLayout split_layout;
    VkDescriptorPool      split_pool;
    VkDescriptorSet       split_set;
    VkPipelineLayout      split_pipe_layout;
    u32                   dummy_cube;
} s_e2;

#define VK_ENG_SAMP_CACHE 64
static struct { VkSamplerCreateInfo info; VkSampler s; } s_esamp[VK_ENG_SAMP_CACHE];
static u32 s_esamp_count;

static void vk_eng_drop_framebuffers(void)
{
    vk_flush_pending(1);
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (s_e2.fb[i].fb) pvkDestroyFramebuffer(s_vk.device, s_e2.fb[i].fb, NULL);
    s_e2.fb_count = 0;
}

/* The render pass for `n` colour attachments of format `cf` plus depth,
 * LOAD/STORE in GENERAL like the vtable path's: clears are separate
 * operations. An MRT set shares target A's format (engine contract). */
static VkRenderPass vk_eng_render_pass(VkFormat cf, u32 n, u32 samples)
{
    const u32 si = samples == 4 ? 2u : samples == 2 ? 1u : 0u;
    const int fi = vk_eng_rt_index(cf);
    if (fi < 0 || n < 1 || n > RSX_BE_MAX_COLOR_TARGETS) return VK_NULL_HANDLE;
    if (s_e2.rp[si][fi][n]) return s_e2.rp[si][fi][n];
    VkAttachmentDescription att[RSX_BE_MAX_COLOR_TARGETS + 1];
    VkAttachmentReference   cref[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i <= n; i++) {
        att[i] = (VkAttachmentDescription){
            .format = (i < n) ? cf : s_vk.depth_format,
            .samples = (VkSampleCountFlagBits)samples,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            /* Stencil lives across draws like depth does (a title writes it in
             * one pass and tests it in the next); ignored for colour and for
             * depth formats without stencil. */
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        if (i < n) cref[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_GENERAL };
    }
    VkAttachmentReference dref = { n, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = n, .pColorAttachments = cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = n + 1, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_e2.rp[si][fi][n]) != VK_SUCCESS)
        s_e2.rp[si][fi][n] = VK_NULL_HANDLE;
    return s_e2.rp[si][fi][n];
}

/* Tables carried from the Metal backend, itself copied from the D3D12 live
 * draw engine a title has shipped on. */
static VkBlendFactor vk_gcm_blend_factor(u32 f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp vk_gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:     return VK_BLEND_OP_ADD;                  /* FUNC_ADD 0x8006 */
    }
}
static VkCompareOp vk_gcm_compare(u32 f)
{
    return (f >= 0x0200u && f <= 0x0206u) ? (VkCompareOp)(f - 0x0200u) : VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp vk_gcm_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;               /* GL_KEEP 0x1E00 */
    }
}
static VkStencilOpState vk_gcm_stencil_face(u32 func, u32 fail, u32 zfail, u32 zpass,
                                            u32 cmp_mask, u32 write_mask)
{
    VkStencilOpState s = { .failOp = vk_gcm_stencil_op(fail), .passOp = vk_gcm_stencil_op(zpass),
                           .depthFailOp = vk_gcm_stencil_op(zfail),
                           .compareOp = vk_gcm_compare(func),
                           .compareMask = cmp_mask & 0xFFu, .writeMask = write_mask & 0xFFu,
                           .reference = 0 };                /* dynamic */
    return s;
}

static int vk_eng_translate(const char* hlsl, int stage, VkShaderModule* out)
{
    u32 words = 0;
    if (rsx_hlsl_to_spirv(hlsl, stage, s_g.spv, VK_SPV_WORDS, &words, s_g.log, sizeof s_g.log) || !words) {
        VK_LOG("engine: %s translation failed: %s\n",
               stage == RSX_SHADER_STAGE_VERTEX ? "vertex" : "fragment", s_g.log);
        return -1;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)words * 4u, .pCode = s_g.spv };
    return pvkCreateShaderModule(s_vk.device, &ci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

/* A pixel program with a cube unit declares all sixteen units one by one
 * (TextureCube or Texture2D rsx_texN : register(tN)) instead of the
 * Texture2D array, and register(tN) is binding RSX_SPIRV_TEXTURE_BINDING + N
 * (rsx_shader_spirv.h): sixteen single bindings, where the shared layout has
 * one binding of sixteen. Those pipelines use this layout, made on first use
 * so a title without cube maps never builds it. */
static VkPipelineLayout vk_eng_split_layout(void)
{
    if (s_e2.split_pipe_layout) return s_e2.split_pipe_layout;
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[3 + RSX_MAX_TEXTURES];
    b[0] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL };
    b[1] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL };
    b[2] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER,
                                           RSX_MAX_TEXTURES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
        b[3 + u] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_TEXTURE_BINDING + u,
                                                   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 3 + RSX_MAX_TEXTURES, .pBindings = b };
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_e2.split_layout) != VK_SUCCESS ||
        pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_e2.split_pool) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_e2.split_pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_e2.split_layout };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_e2.split_layout };
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, &s_e2.split_set) != VK_SUCCESS ||
        pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_e2.split_pipe_layout) != VK_SUCCESS) {
        s_e2.split_pipe_layout = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    return s_e2.split_pipe_layout;
}

/* Which units a pixel program declares as TextureCube. */
static u32 vk_eng_cube_units(const char* ps_hlsl)
{
    u32 mask = 0;
    char decl[40];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        snprintf(decl, sizeof decl, "TextureCube rsx_tex%u ", u);
        if (strstr(ps_hlsl, decl)) mask |= 1u << u;
    }
    return mask;
}

static u32 vk_eng_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl,
                                  const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                  u32 vertex_stride, rsx_be_format rt_fmt, u32 rt_count)
{ vk_flush_pending(0);
    (void)user;
    /* Same gate as Metal: no guest programs, no engine pipelines. */
    if (!s_g.on || !vs_hlsl || !ps_hlsl || !rs || !layout || !vertex_stride) return 0;
    u32 rt_bpp;
    const VkFormat cf = vk_eng_format(rt_fmt, &rt_bpp);
    if (vk_eng_rt_index(cf) < 0) {
        if (!(s_e2.warned & 1u)) { VK_LOG("engine: colour format %d not supported yet\n", (int)rt_fmt); s_e2.warned |= 1u; }
        return 0;
    }
    if (!rt_count) rt_count = 1;
    if (rt_count > RSX_BE_MAX_COLOR_TARGETS) rt_count = RSX_BE_MAX_COLOR_TARGETS;
    u32 slot = VK_ENG_MAX_PIPE;
    for (u32 i = 0; i < VK_ENG_MAX_PIPE; i++) if (!s_epipe[i].used) { slot = i; break; }
    if (slot == VK_ENG_MAX_PIPE) return 0;
    vk_eng_pipe* P = &s_epipe[slot];
    memset(P, 0, sizeof *P);
    if (vk_eng_translate(vs_hlsl, RSX_SHADER_STAGE_VERTEX, &P->vs)) return 0;
    char* scaled_ps=NULL;
    if (s_render_scale!=1) {
        const size_t length=strlen(ps_hlsl);
        if (length>UINT32_MAX-512u) { pvkDestroyShaderModule(s_vk.device,P->vs,NULL); memset(P,0,sizeof *P);return 0; }
        scaled_ps=(char*)malloc(length+512u);
        if (!scaled_ps) { pvkDestroyShaderModule(s_vk.device,P->vs,NULL); memset(P,0,sizeof *P);return 0; }
        memcpy(scaled_ps,ps_hlsl,length+1u);
        u32 quality_w=0,quality_h=0;
        const int quality=rs->depth_test && vk_eng_quality_world(1280,720,2,&quality_w,&quality_h);
        if (rsx_fp_apply_resolution_scale(scaled_ps,(u32)length+512u,quality?1280:1,quality?720:1,quality?quality_w:s_render_scale,quality?quality_h:s_render_scale)<0) {
            free(scaled_ps);pvkDestroyShaderModule(s_vk.device,P->vs,NULL);memset(P,0,sizeof *P);return 0;
        }
    }
    const int ps_failed=vk_eng_translate(scaled_ps?scaled_ps:ps_hlsl,RSX_SHADER_STAGE_FRAGMENT,&P->fs);
    free(scaled_ps);
    if (ps_failed) {
        pvkDestroyShaderModule(s_vk.device, P->vs, NULL); memset(P, 0, sizeof *P); return 0;
    }
    P->rs = *rs;
    P->nslots = layout->count;
    P->stride = vertex_stride;
    P->rt_count = rt_count;
    P->rt_fmt = cf;
    P->cube_mask = vk_eng_cube_units(ps_hlsl);
    P->folklore_composite = s_render_scale > 1 && rsx_folklore_composite_program(ps_hlsl);
    if(s_vk.rt_enabled) {
        const size_t vl=strlen(vs_hlsl),pl=strlen(ps_hlsl);
        if(vl<262144 && pl<262144) {
            P->scene_vs=(char*)malloc(vl+1);P->scene_ps=(char*)malloc(pl+1);
            if(P->scene_vs && P->scene_ps) {
                memcpy(P->scene_vs,vs_hlsl,vl+1);memcpy(P->scene_ps,ps_hlsl,pl+1);
                P->scene_layout=*layout;
                P->scene_alpha_contract=vk_rt_shader_contract(ps_hlsl,&P->scene_alpha_mask,&P->scene_alpha_fp_bytes);
                vk_rt_source_sha256(vs_hlsl,P->scene_vs_sha256);
                vk_rt_source_sha256(ps_hlsl,P->scene_ps_sha256);
            } else {free(P->scene_vs);free(P->scene_ps);P->scene_vs=P->scene_ps=NULL;}
        }
    }
    P->used = 1;
    return slot + 1u;
}

static void vk_eng_pipeline_release(void* user, u32 pipeline)
{ vk_flush_pending(0);
    (void)user;
    if (!pipeline || pipeline > VK_ENG_MAX_PIPE || !s_epipe[pipeline - 1].used) return;
    vk_eng_pipe* P = &s_epipe[pipeline - 1];
    if(s_vk.rt_enabled)vk_rt_capture_pipeline(pipeline);
    free(P->scene_vs);free(P->scene_ps);
    for (u32 si = 0; si < 3; si++)
        for (u32 t = 0; t < VK_ENG_TOPOLOGIES; t++)
            if (P->variant[si][t]) pvkDestroyPipeline(s_vk.device, P->variant[si][t], NULL);
    if (P->vs) pvkDestroyShaderModule(s_vk.device, P->vs, NULL);
    if (P->fs) pvkDestroyShaderModule(s_vk.device, P->fs, NULL);
    memset(P, 0, sizeof *P);
    if (s_e2.pipeline == pipeline) s_e2.pipeline = 0;
}

static VkPipeline vk_eng_variant(vk_eng_pipe* P, u32 topo_idx, u32 samples)
{
    const u32 si = samples == 4 ? 2u : samples == 2 ? 1u : 0u;
    static const VkPrimitiveTopology topo[VK_ENG_TOPOLOGIES] = {
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    if (P->variant[si][topo_idx]) return P->variant[si][topo_idx];
    VkRenderPass rp = vk_eng_render_pass(P->rt_fmt, P->rt_count, samples);
    if (!rp || P->nslots > VK_GUEST_ATTRS) return VK_NULL_HANDLE;
    const VkPipelineLayout layout = P->cube_mask ? vk_eng_split_layout() : s_g.pipe_layout;
    if (!layout) return VK_NULL_HANDLE;
    const rsx_be_render_state* rs = &P->rs;

    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = P->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = P->fs, .pName = "main" } };
    VkVertexInputBindingDescription vb = { 0, P->stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 s = 0; s < P->nslots; s++)
        va[s] = (VkVertexInputAttributeDescription){ s, 0, VK_FORMAT_R32G32B32A32_SFLOAT, s * 16u };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = P->nslots, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topo[topo_idx] };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* CULL_FACE FRONT 0x0404 / BACK 0x0405 / FRONT_AND_BACK 0x0408 (front, as
     * in D3D12 and Metal); FRONT_FACE CCW 0x0901 maps directly: both APIs
     * judge winding on screen, and the Y flip in the vertex stage makes this
     * screen the same one D3D's viewport produces. */
    VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = !rs->cull_enable ? VK_CULL_MODE_NONE
                  : rs->cull_face == 0x0408u ? VK_CULL_MODE_FRONT_AND_BACK
                  : rs->cull_face == 0x0404u ? VK_CULL_MODE_FRONT_BIT
                                           : VK_CULL_MODE_BACK_BIT,
        .frontFace = (rs->front_face == 0x0901u) ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                 : VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f };
    /* Same raw control mapping as pinned RPCS3 graphics_pipeline_state.
     * The MSAA enable bit does not change the attachment's sample count. */
    const VkSampleMask sample_mask = rs->antialias_control >> 16;
    const VkBool32 alpha_coverage = (rs->antialias_control & 0x10u) != 0;
    const VkBool32 alpha_one = (rs->antialias_control & 0x100u) != 0;
    if (alpha_one && !s_vk.alpha_to_one_ok) {
        ps3_strict_boot_guard("vulkan-alpha-to-one", "required device feature unavailable");
        return VK_NULL_HANDLE;
    }
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = (VkSampleCountFlagBits)samples,
        .pSampleMask = &sample_mask,
        .alphaToCoverageEnable = alpha_coverage,
        .alphaToOneEnable = alpha_one };
    VkStencilOpState front = vk_gcm_stencil_face(rs->s_func, rs->s_fail, rs->s_zfail, rs->s_zpass,
                                                 rs->s_func_mask, rs->s_write_mask);
    VkStencilOpState back = rs->stencil_two_sided
        ? vk_gcm_stencil_face(rs->bs_func, rs->bs_fail, rs->bs_zfail, rs->bs_zpass,
                              rs->s_func_mask, rs->s_write_mask)
        : front;
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = rs->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (rs->depth_test && rs->depth_write) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = vk_gcm_compare(rs->depth_func),
        .stencilTestEnable = rs->stencil_enable ? VK_TRUE : VK_FALSE,
        .front = front, .back = back };
    /* nv40 COLOR_MASK: B [0:7], G [8:15], R [16:23], A [24:31], any bit set.
     * Every attachment takes target A's blend and mask (see rsx_draw_engine.h). */
    VkColorComponentFlags wm = 0;
    if ((rs->color_mask >> 16) & 0xFFu) wm |= VK_COLOR_COMPONENT_R_BIT;
    if ((rs->color_mask >>  8) & 0xFFu) wm |= VK_COLOR_COMPONENT_G_BIT;
    if ((rs->color_mask >>  0) & 0xFFu) wm |= VK_COLOR_COMPONENT_B_BIT;
    if ((rs->color_mask >> 24) & 0xFFu) wm |= VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState cba[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 r = 0; r < P->rt_count; r++)
        cba[r] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = rs->blend_enable ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = vk_gcm_blend_factor(rs->sf_rgb),
            .dstColorBlendFactor = vk_gcm_blend_factor(rs->df_rgb),
            .colorBlendOp = vk_gcm_blend_op(rs->eq_rgb),
            .srcAlphaBlendFactor = vk_gcm_blend_factor(rs->sf_a),
            .dstAlphaBlendFactor = vk_gcm_blend_factor(rs->df_a),
            .alphaBlendOp = vk_gcm_blend_op(rs->eq_a),
            .colorWriteMask = wm };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = P->rt_count, .pAttachments = cba };
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = layout, .renderPass = rp, .subpass = 0 };
    if (vk_create_graphics_timed("engine", (u32)(P-s_epipe)+1, s_vk.device, vk_pcache(), 1, &gci, NULL,
                                   &P->variant[si][topo_idx]) != VK_SUCCESS) {
        VK_LOG("engine: vkCreateGraphicsPipelines failed\n");
        P->variant[si][topo_idx] = VK_NULL_HANDLE;
    } else {
        vk_pcache_created();
    }
    return P->variant[si][topo_idx];
}

#include "rsx_vulkan_msaa.inc"
#include "rsx_vulkan_msaa_upload.inc"
#include "rsx_vulkan_depth_argb.inc"
#ifdef PS3RECOMP_RSX_CACAO
#include "rsx_vulkan_ao_pass.inc"
#include "rsx_vulkan_cacao.inc"
#endif

static void vk_eng_bind_pipeline(void* user, u32 pipeline) { (void)user; s_e2.pipeline = pipeline; }

static void vk_eng2_shutdown(void* user)
{
    vk_flush_pending(0);
    if (s_batch.pool) pvkDestroyDescriptorPool(s_vk.device, s_batch.pool, NULL);
    vk_destroy_buffer(&s_batch.ubo);
    vk_destroy_buffer(&s_batch.ring);
    { const int on = s_batch.on, configured = s_batch.configured, stats = s_batch.stats, defer = s_batch.defer,
                merge = s_batch.merge;
      memset(&s_batch, 0, sizeof s_batch);
      s_batch.on = on; s_batch.configured = configured; s_batch.stats = stats; s_batch.defer = defer;
      s_batch.merge = merge; }
    for (u32 i = 1; i <= VK_ENG_MAX_PIPE; i++) vk_eng_pipeline_release(user, i);
    vk_eng_drop_framebuffers();
    for (u32 si = 0; si < 3; si++)
        for (u32 f = 0; f < VK_ENG_RT_FORMATS; f++)
            for (u32 n = 0; n <= RSX_BE_MAX_COLOR_TARGETS; n++)
                if (s_e2.rp[si][f][n]) pvkDestroyRenderPass(s_vk.device, s_e2.rp[si][f][n], NULL);
    vk_destroy_image(&s_e2.scratch_depth);
    vk_destroy_buffer(&s_e2.indices);
    free(s_e2.constant_shadow);
    if (s_e2.split_pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_e2.split_pipe_layout, NULL);
    if (s_e2.split_pool)        pvkDestroyDescriptorPool(s_vk.device, s_e2.split_pool, NULL);
    if (s_e2.split_layout)      pvkDestroyDescriptorSetLayout(s_vk.device, s_e2.split_layout, NULL);
    memset(&s_e2, 0, sizeof s_e2);
    for (u32 i = 0; i < s_esamp_count; i++) pvkDestroySampler(s_vk.device, s_esamp[i].s, NULL);
    s_esamp_count = 0;
}

/* Keep constants in ordinary CPU memory. Reading a HOST_VISIBLE but uncached
 * GPU allocation to fill every batch slot can cause expensive PCIe reads.
 * Both batched and fallback draws upload from this owned snapshot; callers
 * may therefore reuse their input immediately after bind returns. */
static int vk_eng_constant_shadow(void)
{
    if (!s_g.ubo.ptr || !s_g.ubo.size || s_g.ubo.size > SIZE_MAX) return 0;
    const size_t bytes = (size_t)s_g.ubo.size;
    if (s_e2.constant_shadow && s_e2.constant_shadow_size == bytes) return 1;
    u8* p = (u8*)realloc(s_e2.constant_shadow, bytes);
    if (!p) {
        ps3_strict_boot_guard("vulkan-constant-shadow", "CPU constant allocation failed");
        return 0;
    }
    if (bytes > s_e2.constant_shadow_size)
        memset(p + s_e2.constant_shadow_size, 0, bytes - s_e2.constant_shadow_size);
    s_e2.constant_shadow = p;
    s_e2.constant_shadow_size = bytes;
    return 1;
}

static void vk_eng_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.vs_bytes = 0; return; }
    const u32 cap = (u32)s_g.ps_off;
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 2u)) { VK_LOG("engine: VS constants truncated\n"); s_e2.warned |= 2u; } }
    if (!vk_eng_constant_shadow()) { s_e2.vs_bytes = 0; return; }
    memcpy(s_e2.constant_shadow, data, bytes);
    s_e2.vs_bytes = bytes;
}
static void vk_eng_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.ps_bytes = 0; return; }
    const u32 cap = (u32)(s_g.ubo.size - s_g.ps_off);
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 4u)) { VK_LOG("engine: PS constants truncated\n"); s_e2.warned |= 4u; } }
    if (!vk_eng_constant_shadow()) { s_e2.ps_bytes = 0; return; }
    memcpy(s_e2.constant_shadow + s_g.ps_off, data, bytes);
    s_e2.ps_bytes = bytes;
    /* The engine binds the pipeline first, so its composite mark is current. */
    if (s_e2.pipeline && s_e2.pipeline <= VK_ENG_MAX_PIPE &&
        s_epipe[s_e2.pipeline - 1].folklore_composite) {
        static int enabled = -1;
        static u64 applied;
        if (enabled < 0) {
            const char* e = getenv("FOLKLORE_COMPOSITE_SHARP");
            enabled = !(e && e[0] == '0' && !e[1]);
            VK_LOG("Folklore composite: in-focus resolve floor %s at render scale %u\n",
                   enabled ? "follows host pixels" : "kept in guest pixels (FOLKLORE_COMPOSITE_SHARP=0)",
                   s_render_scale);
        }
        if (enabled && rsx_folklore_composite_sharpen(s_e2.constant_shadow + s_g.ps_off, bytes,
                                                      (float)s_render_scale) && ++applied == 1)
            VK_LOG("Folklore composite: floor 0.3 -> %.4f guest pixels\n", 0.3 / s_render_scale);
    }
}
static void vk_eng_bind_textures(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        const int on = (mask >> u) & 1u;
        s_e2.tex[u] = on ? textures[u] : 0;
        if (on && samplers) s_e2.samp[u] = samplers[u];
    }
    s_e2.tex_mask = mask;
}
static void vk_eng_bind_vertex_textures(void* user, const u32* textures,
                                        const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user; (void)textures; (void)samplers;
    if (mask && !(s_e2.warned & 8u)) { VK_LOG("engine: vertex textures not supported yet\n"); s_e2.warned |= 8u; }
}
static void vk_eng_set_viewport(void* user, float x, float y, float w, float h,
                                float min_depth, float max_depth)
{
    (void)user;
    float sx,sy;vk_eng_bound_scale(&sx,&sy);
    s_e2.vp[0] = x*sx; s_e2.vp[1] = y*sy; s_e2.vp[2] = w*sx; s_e2.vp[3] = h*sy; s_e2.have_vp = 1;
    s_e2.vp[4] = min_depth; s_e2.vp[5] = max_depth;
}
static void vk_eng_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    float sx,sy;vk_eng_bound_scale(&sx,&sy);
    double x0=floor((double)x*sx),y0=floor((double)y*sy),x1=ceil(((double)x+w)*sx),y1=ceil(((double)y+h)*sy);
    s_e2.sc[0]=(u32)fmin(x0,UINT32_MAX);s_e2.sc[1]=(u32)fmin(y0,UINT32_MAX);
    s_e2.sc[2]=(u32)fmin(x1-x0,UINT32_MAX);s_e2.sc[3]=(u32)fmin(y1-y0,UINT32_MAX);s_e2.have_sc=1;
}
static void vk_eng_set_stencil_ref(void* user, u32 ref) { (void)user; s_e2.stencil_ref = ref; }

static VkFramebuffer vk_eng_framebuffer(VkRenderPass rp, const u32* rt, u32 n, VkImageView dview,
                                        u32 dkey, u32 w, u32 h)
{
    u32 key[RSX_BE_MAX_COLOR_TARGETS + 1] = { 0 };
    for (u32 i = 0; i < n; i++) key[i] = rt[i];
    key[RSX_BE_MAX_COLOR_TARGETS] = dkey;
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (!memcmp(s_e2.fb[i].key, key, sizeof key)) return s_e2.fb[i].fb;
    if (s_e2.fb_count >= VK_ENG_FB_CACHE) vk_eng_drop_framebuffers();
    VkImageView views[RSX_BE_MAX_COLOR_TARGETS + 1];
    for (u32 i = 0; i < n; i++) views[i] = s_eobj[rt[i]].im.view;
    views[n] = dview;
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = rp, .attachmentCount = n + 1, .pAttachments = views,
                                    .width = w, .height = h, .layers = 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    memcpy(s_e2.fb[s_e2.fb_count].key, key, sizeof key);
    s_e2.fb[s_e2.fb_count++].fb = fb;
    return fb;
}

/* Depth when the draw names no zeta: one scratch image, grown as needed and
 * cleared to the far plane when (re)created. Key 0xFFFFFFFF in the cache. */
static VkImageView vk_eng_scratch_depth(u32 w, u32 h, u32 samples)
{
    if (s_e2.scratch_depth.view && s_e2.scratch_w >= w && s_e2.scratch_h >= h && s_e2.scratch_samples == samples)
        return s_e2.scratch_depth.view;
    vk_eng_drop_framebuffers();
    vk_destroy_image(&s_e2.scratch_depth);
    if (vk_create_image_levels_samples(&s_e2.scratch_depth, s_vk.depth_format, w, h, 1,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        s_vk.depth_aspect, samples) || vk_begin())
        return VK_NULL_HANDLE;
    vk_barrier_image(s_e2.scratch_depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_e2.scratch_depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (vk_submit_and_wait()) return VK_NULL_HANDLE;
    s_e2.scratch_w = w; s_e2.scratch_h = h; s_e2.scratch_samples = samples;
    return s_e2.scratch_depth.view;
}

static int vk_eng_ensure_indices(VkDeviceSize bytes)
{
    if (s_e2.indices.buf && s_e2.indices.size >= bytes) return 0;
    VkDeviceSize size = s_e2.indices.size ? s_e2.indices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_e2.indices);
    if (vk_create_host_buffer(&s_e2.indices, size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return -1;
    return s_e2.indices.coherent ? 0 : -1;
}

/* rsx_be_sampler_desc is the registers decoded the D3D12 way; the wrap codes
 * stay the guest's 1..8, which vk_gcm_wrap already translates. */
/* DLSS rebuilds the world at the output extent from fewer pixels, so draws
 * into the DLSS world take NGX's texture mip bias log2(input/output width):
 * their textures keep the detail of the output resolution instead of the
 * input's. PS3RECOMP_DLSS_MIP_BIAS=0 turns it off. */
static float vk_eng_world_lod_bias(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* e = getenv("PS3RECOMP_DLSS_MIP_BIAS");
        enabled = !(e && e[0] == '0' && !e[1]);
    }
    const u32 handle = s_eng_bound.nrt ? s_eng_bound.rt[0] : s_eng_bound.depth;
    u32 iw = 0, ih = 0;
    if (!enabled || !handle || handle >= VK_ENG_MAX_OBJ || !s_eobj[handle].dlss_world ||
        !vk_dlss_world_extent(&iw, &ih) || !iw)
        return 0.0f;
    const float bias = log2f((float)iw / (float)(1280u * s_render_scale));
    static int logged;
    if (!logged) { logged = 1; VK_LOG("DLSS world texture mip bias %.3f (input width %u)\n", bias, iw); }
    return bias < 0.0f ? bias : 0.0f;
}

static VkSampler vk_eng_sampler(const rsx_be_sampler_desc* d, float lod_bias)
{
    float min_lod = d->min_lod, max_lod = d->mip_present ? d->max_lod : d->min_lod;
    if (max_lod < min_lod) max_lod = min_lod;
    if (!d->mip_present || d->vertex_sampler) lod_bias = 0.0f;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = d->mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = d->min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = d->mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(d->wrap_s), .addressModeV = vk_gcm_wrap(d->wrap_t),
        .addressModeW = vk_gcm_wrap(d->wrap_r),
        .mipLodBias = lod_bias,
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
    rsx_vk_anisotropy_apply(&sci, d->vertex_sampler ? 0u : d->anisotropy_code,
                             s_vk.anisotropy_override, s_vk.anisotropy_ok,
                             s_vk.max_anisotropy, !d->vertex_sampler);
    /* Compare effective creation values, including every LOD bit, without
     * struct padding or a lossy packed key. Equivalent wraps and disabled/
     * clamped mip ranges should reuse the same successful sampler. */
    for (u32 i = 0; i < s_esamp_count; i++) {
        const VkSamplerCreateInfo* p = &s_esamp[i].info;
        if (p->magFilter == sci.magFilter && p->minFilter == sci.minFilter &&
            p->mipmapMode == sci.mipmapMode && p->mipLodBias == sci.mipLodBias &&
            p->addressModeU == sci.addressModeU &&
            p->addressModeV == sci.addressModeV &&
            p->addressModeW == sci.addressModeW &&
            p->minLod == sci.minLod && p->maxLod == sci.maxLod &&
            p->anisotropyEnable == sci.anisotropyEnable && p->maxAnisotropy == sci.maxAnisotropy &&
            p->borderColor == sci.borderColor)
            return s_esamp[i].s;
    }
    if (s_esamp_count >= VK_ENG_SAMP_CACHE) return s_vk.sampler;
    VkSampler smp;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return s_vk.sampler;
    s_esamp[s_esamp_count].info = sci;
    s_esamp[s_esamp_count++].s = smp;
    return smp;
}

/* The view a bound engine handle samples through: an uploaded texture, a
 * colour target (bound directly when surface_view was not asked), or a view.
 * A unit gets a view of its own kind only -- a cube for a TextureCube unit, a
 * 2D one otherwise -- and NULL for anything else, which then samples the
 * dummy of the unit's kind. */
static VkImageView vk_eng_sample_view(u32 handle, int want_cube)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return VK_NULL_HANDLE;
    const vk_eng_obj* o = &s_eobj[handle];
    if (o->kind != VK_ENG_TEXTURE && o->kind != VK_ENG_COLOR && o->kind != VK_ENG_VIEW)
        return VK_NULL_HANDLE;
    return ((int)o->cube == want_cube) ? o->im.view : VK_NULL_HANDLE;
}

/* What a cube unit with nothing bound samples: a 1x1 cube, magenta on every
 * face like the 2D dummy, made on first use. */
static VkImageView vk_eng_dummy_cube_view(void)
{
    if (!vk_eng_get(s_e2.dummy_cube, VK_ENG_TEXTURE)) {
        static const u8 magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
        s_e2.dummy_cube = vk_eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, 1, 1, 1, 6,
                                                0xAAE4u, 0x85u);
        if (!s_e2.dummy_cube) return VK_NULL_HANDLE;
        for (u32 f = 0; f < 6; f++)
            vk_eng_texture_upload(NULL, s_e2.dummy_cube, f, 0, 1, 1, magenta, 4, 1);
    }
    return s_eobj[s_e2.dummy_cube].im.view;
}

/* Fill the set the bound pipeline uses: the shared one (sixteen textures in
 * one binding), or for a pipeline with cube units the split one (one binding
 * per unit). Returns the set, or VK_NULL_HANDLE. */
static VkDescriptorSet vk_eng_write_descriptors_into(const vk_eng_pipe* P, VkDescriptorSet set,
                                                     VkBuffer ubo, VkDeviceSize vs_off, VkDeviceSize ps_off)
{
    const int split = P->cube_mask != 0;
    if (!set) return VK_NULL_HANDLE;
    const VkImageView dummy_cube = split ? vk_eng_dummy_cube_view() : VK_NULL_HANDLE;
    if (split && !dummy_cube) return VK_NULL_HANDLE;
    VkDescriptorBufferInfo bi[2] = {
        { ubo, vs_off, s_e2.vs_bytes ? s_e2.vs_bytes : 16u },
        { ubo, ps_off, s_e2.ps_bytes ? s_e2.ps_bytes : 16u },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    const float lod_bias = vk_eng_world_lod_bias();
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const int cube = (P->cube_mask >> u) & 1u;
        VkImageView v = ((s_e2.tex_mask >> u) & 1u) ? vk_eng_sample_view(s_e2.tex[u], cube)
                                                     : VK_NULL_HANDLE;
        VkSampler smp = v ? vk_eng_sampler(&s_e2.samp[u], lod_bias) : s_vk.sampler;
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
                                         v ? v : (cube ? dummy_cube : s_vk.dummy.view),
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[3 + RSX_MAX_TEXTURES];
    u32 n = 0;
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] };
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] };
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si };
    if (split) {
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
            w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                  .dstSet = set, .dstBinding = RSX_SPIRV_TEXTURE_BINDING + u, .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii[u] };
    } else {
        w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
              .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii };
    }
    pvkUpdateDescriptorSets(s_vk.device, n, w, 0, NULL);
    return set;
}

/* The shared set and UBO: only for an unbatched draw, which is waited on. */
static VkDescriptorSet vk_eng_write_descriptors(const vk_eng_pipe* P)
{
    const VkDescriptorSet set = P->cube_mask ? s_e2.split_set : s_g.set;
    return vk_eng_write_descriptors_into(P, set, s_g.ubo.buf, 0, s_g.ps_off);
}

/* Lazily create the batch constants ring and descriptor pool. */
static int vk_batch_ready(void)
{
    if (s_batch.failed) return 0;
    if (s_batch.pool) return 1;
    VkPhysicalDeviceProperties props;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &props);
    s_batch.ualign = props.limits.minUniformBufferOffsetAlignment ? props.limits.minUniformBufferOffsetAlignment : 256u;
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2u * VK_BATCH_SETS },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES * VK_BATCH_SETS },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES * VK_BATCH_SETS },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = VK_BATCH_SETS, .poolSizeCount = 3, .pPoolSizes = ps };
    if (vk_create_host_buffer(&s_batch.ubo, VK_BATCH_UBO_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !s_batch.ubo.coherent ||
        pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_batch.pool) != VK_SUCCESS) {
        VK_LOG("engine draw batching unavailable; one wait per draw\n");
        s_batch.pool = VK_NULL_HANDLE;
        s_batch.failed = 1;
        return 0;
    }
    return 1;
}

static void vk_dlss_reactive_before_draw(u32 target,const vk_eng_pipe *pipe);
static void vk_eng_draw(void* user, rsx_topology topology, const void* vertices, u32 vertex_count,
                        u32 stride, const u32* indices, u32 index_count)
{
    (void)user;
    if (!s_e2.pipeline || !vertices || !vertex_count || !stride) return;
    if (topology < RSX_TOPOLOGY_POINTS || topology > RSX_TOPOLOGY_TRIANGLE_STRIP) return;
    vk_eng_pipe* P = &s_epipe[s_e2.pipeline - 1];
    if (!P->used || stride != P->stride) return;

    /* Targets: every member of a set is target A's size (engine contract). */
    const u32 n = s_eng_bound.nrt;
    if (n < 1 || n != P->rt_count) {
        if (!(s_e2.warned & 16u)) { VK_LOG("engine: draw with %u colour targets for a %u-target pipeline skipped\n", n, P->rt_count); s_e2.warned |= 16u; }
        return;
    }
    vk_eng_obj* rt0 = vk_eng_get(s_eng_bound.rt[0], VK_ENG_COLOR);
    if (!rt0) return;
    for (u32 i = 1; i < n; i++)
        if (!vk_eng_get(s_eng_bound.rt[i], VK_ENG_COLOR)) return;
    if (rt0->fmt != P->rt_fmt) {
        if (!(s_e2.warned & 32u)) { VK_LOG("engine: draw into colour format %d with a pipeline built for %d skipped\n", (int)rt0->fmt, (int)P->rt_fmt); s_e2.warned |= 32u; }
        return;
    }
    const u32 w = rt0->w, h = rt0->h;
    const u32 samples = rt0->samples ? rt0->samples : 1u;
    for (u32 i = 1; i < n; ++i) {
        const vk_eng_obj* other = &s_eobj[s_eng_bound.rt[i]];
        if ((other->samples ? other->samples : 1u) != samples) {
            ps3_strict_boot_guard("vulkan-msaa-mrt", "attachment sample counts differ"); return;
        }
    }
    vk_eng_obj* zo = vk_eng_get(s_eng_bound.depth, VK_ENG_DEPTH);
    VkImageView dview = (zo && zo->w >= w && zo->h >= h && (zo->samples ? zo->samples : 1u) == samples) ? zo->im.view : vk_eng_scratch_depth(w, h, samples);
    if (!dview) return;
    const u32 dkey = (zo && dview == zo->im.view) ? s_eng_bound.depth : 0xFFFFFFFFu;

    VkPipeline pipe = vk_eng_variant(P, (u32)topology - 1u, samples);
    VkRenderPass rp = vk_eng_render_pass(P->rt_fmt, n, samples);
    VkFramebuffer fb = vk_eng_framebuffer(rp, s_eng_bound.rt, n, dview, dkey, w, h);
    if (!pipe || !rp || !fb) {
        if (samples > 1) ps3_strict_boot_guard("vulkan-msaa-pipeline", "multisample pipeline/framebuffer failed");
        return;
    }
    for (u32 i = 0; i < n; ++i) s_eobj[s_eng_bound.rt[i]].expansion_valid = 0;
    if (zo) zo->expansion_valid = 0;

    const VkDeviceSize vbytes = (VkDeviceSize)vertex_count * stride;
    const VkDeviceSize ibytes = (indices && index_count) ? (VkDeviceSize)index_count * 4u : 0;
    /* Before this draw is recorded: the reactive layer snapshot (DLSS). */
    if (s_vk.rt_enabled) vk_dlss_reactive_before_draw(s_eng_bound.rt[0], P);
    vk_batch_configure();
    if (!vk_eng_constant_shadow()) return;
    const int batched = s_batch.on && vk_batch_ready() && s_g.ubo.ptr;
    VkDeviceSize voff = 0, ioff = 0, uvs = 0, ups = 0;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (batched) {
        const VkDeviceSize a = s_batch.ualign;
        const VkDeviceSize vs_range = s_e2.vs_bytes ? s_e2.vs_bytes : 16u;
        const VkDeviceSize ps_range = s_e2.ps_bytes ? s_e2.ps_bytes : 16u;
        const VkDeviceSize ubytes = (vs_range + a - 1u) / a * a + (ps_range + a - 1u) / a * a;
        /* Create the cube dummy now: its creation flushes, which must not
         * happen after this draw has reserved ring space and a set. */
        if (P->cube_mask && !vk_eng_dummy_cube_view()) return;
        if (!s_batch.pending) s_batch.vb = s_batch.ib = s_batch.ub = 0;
        /* Reserve everything up front, flushing once if anything is short. */
        const int fits = s_vk.vertices.buf && s_vk.vertices.size >= VK_BATCH_VB_MIN &&
                         s_batch.vb + vbytes <= s_vk.vertices.size &&
                         (!ibytes || (s_e2.indices.buf && s_e2.indices.size >= VK_BATCH_IB_MIN &&
                                      s_batch.ib + ibytes <= s_e2.indices.size)) &&
                         s_batch.ub + ubytes <= s_batch.ubo.size &&
                         s_batch.sets < VK_BATCH_SETS;
        if (!fits) vk_flush_pending(2);
        if (vk_ensure_vertex_buffer(s_batch.vb + vbytes > VK_BATCH_VB_MIN ? s_batch.vb + vbytes : VK_BATCH_VB_MIN)) return;
        if (ibytes && vk_eng_ensure_indices(s_batch.ib + ibytes > VK_BATCH_IB_MIN ? s_batch.ib + ibytes : VK_BATCH_IB_MIN)) return;
        if (s_batch.ub + ubytes > s_batch.ubo.size) return;   /* a single draw larger than the ring */
        voff = s_batch.vb; ioff = s_batch.ib;
        uvs = s_batch.ub; ups = uvs + (vs_range + a - 1u) / a * a;
        const VkDescriptorSetLayout layout = P->cube_mask ? s_e2.split_layout : s_g.set_layout;
        VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = s_batch.pool, .descriptorSetCount = 1,
                                            .pSetLayouts = &layout };
        if (!layout || pvkAllocateDescriptorSets(s_vk.device, &dai, &set) != VK_SUCCESS) {
            set = VK_NULL_HANDLE;
        } else {
            s_batch.sets++;
            memcpy((u8*)s_batch.ubo.ptr + uvs, s_e2.constant_shadow, (size_t)vs_range);
            vk_dlss_jitter_upload((u8*)s_batch.ubo.ptr+uvs,(size_t)vs_range);
            memcpy((u8*)s_batch.ubo.ptr + ups, s_e2.constant_shadow + s_g.ps_off, (size_t)ps_range);
            if (!vk_eng_write_descriptors_into(P, set, s_batch.ubo.buf, uvs, ups)) return;
            s_batch.ub = ups + (ps_range + a - 1u) / a * a;
            s_batch.vb = (voff + vbytes + 15u) & ~(VkDeviceSize)15u;
            s_batch.ib = (ioff + ibytes + 15u) & ~(VkDeviceSize)15u;
        }
    }
    if (!set) {
        /* Unbatched: shared set and UBO, so nothing may still be pending. */
        if (batched) vk_flush_pending(3);
        voff = ioff = 0;
        if (vk_ensure_vertex_buffer(vbytes)) return;
        if (ibytes && vk_eng_ensure_indices(ibytes)) return;
        memcpy(s_g.ubo.ptr, s_e2.constant_shadow, s_e2.vs_bytes ? s_e2.vs_bytes : 16u);
        vk_dlss_jitter_upload(s_g.ubo.ptr,s_e2.vs_bytes);
        memcpy((u8*)s_g.ubo.ptr + s_g.ps_off, s_e2.constant_shadow + s_g.ps_off,
               s_e2.ps_bytes ? s_e2.ps_bytes : 16u);
        set = vk_eng_write_descriptors(P);
        if (!set) return;
    }
    memcpy((u8*)s_vk.vertices.ptr + voff, vertices, (size_t)vbytes);
    if (ibytes) memcpy((u8*)s_e2.indices.ptr + ioff, indices, (size_t)ibytes);
    const int deferred = batched && set != s_g.set && set != s_e2.split_set;

    /* Continue the open render pass when this draw goes into the same targets
     * and samples none of them: the attachments are then written in primitive
     * order inside one subpass, as the per-draw barrier ordered them before,
     * and nothing else was written since that pass began. */
    int cont = deferred && s_batch.merge && s_rp.open && s_batch.recording && s_batch.pending &&
               s_rp.rp == rp && s_rp.fb == fb && s_rp.w == w && s_rp.h == h;
    for (u32 u = 0; cont && u < RSX_MAX_TEXTURES; u++) {
        if (!((s_e2.tex_mask >> u) & 1u)) continue;
        const u32 t = s_e2.tex[u];
        if (!t || t >= VK_ENG_MAX_OBJ) continue;
        const u32 img = s_eobj[t].kind == VK_ENG_VIEW ? s_eobj[t].surface : t;
        for (u32 i = 0; i < n; i++)
            if (img == s_eng_bound.rt[i]) cont = 0;
    }
    if (cont) {
        s_batch.merged++;
    } else {
        if (vk_begin()) return;
        if (deferred) {
            /* Batched draws run in one command buffer: keep each draw after the
             * previous one's writes, as separate waited submissions did. */
            VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                   .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
            pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        }
        /* Orders memory for the surfaces this draw touches. */
        for (u32 i = 0; i < n; i++)
            vk_barrier_image(s_eobj[s_eng_bound.rt[i]].im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                      .renderPass = rp, .framebuffer = fb,
                                      .renderArea = { { 0, 0 }, { w, h } } };
        pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    }
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    if (s_e2.have_vp && s_e2.vp[2] > 0.0f && s_e2.vp[3] > 0.0f) {
        vp.x = s_e2.vp[0]; vp.y = s_e2.vp[1]; vp.width = s_e2.vp[2]; vp.height = s_e2.vp[3];
        vp.minDepth = s_e2.vp[4]; vp.maxDepth = s_e2.vp[5];
    }
    VkRect2D sc = { { 0, 0 }, { w, h } };
    if (s_e2.have_sc) {                       /* clamped: Vulkan rejects negative or over-size */
        u32 x0 = s_e2.sc[0] < w ? s_e2.sc[0] : w, y0 = s_e2.sc[1] < h ? s_e2.sc[1] : h;
        u32 x1 = s_e2.sc[0]>w || s_e2.sc[2]>w-s_e2.sc[0] ? w : s_e2.sc[0]+s_e2.sc[2];
        u32 y1 = s_e2.sc[1]>h || s_e2.sc[3]>h-s_e2.sc[1] ? h : s_e2.sc[1]+s_e2.sc[3];
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        sc.offset.x = (int32_t)x0; sc.offset.y = (int32_t)y0;
        sc.extent.width = x1 > x0 ? x1 - x0 : 0; sc.extent.height = y1 > y0 ? y1 - y0 : 0;
    }
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdSetStencilReference(s_vk.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s_e2.stencil_ref & 0xFFu);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             P->cube_mask ? s_e2.split_pipe_layout : s_g.pipe_layout,
                             0, 1, &set, 0, NULL);
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_vk.vertices.buf, &voff);
    if (ibytes) {
        pvkCmdBindIndexBuffer(s_vk.cmd, s_e2.indices.buf, ioff, VK_INDEX_TYPE_UINT32);
        pvkCmdDrawIndexed(s_vk.cmd, index_count, 1, 0, 0, 0);
    } else {
        pvkCmdDraw(s_vk.cmd, vertex_count, 1, 0, 0);
    }
    if(s_vk.rt_enabled)vk_rt_capture_draw(s_e2.pipeline,topology,vertices,vertex_count,stride,indices,index_count);
    s_batch.draws++;
    if (deferred && s_batch.merge) {
        s_rp.open = 1; s_rp.rp = rp; s_rp.fb = fb; s_rp.w = w; s_rp.h = h;
    } else {
        pvkCmdEndRenderPass(s_vk.cmd);
    }
    if (deferred) {
        if (s_batch.trace) { s_batch.trace--; VK_LOG("batch-trace: deferred draw pending=%u\n", s_batch.pending + 1); }
        if (++s_batch.pending >= s_batch.max) vk_submit_and_wait();
        return;
    }
    vk_submit_and_wait();
}

/* Dispatch only the explicitly selected algorithm. Any ray-query failure
 * returns zero and the compositor retains the original guest world color. */
static u32 vk_eng_world_ao(void* user,u32 color,u32 depth,u32 logical_w,u32 logical_h,const float projection[16],u32 remap,u32 rsx_format,float radius,float strength)
{
    if(s_vk.rt_requested)return vk_eng_ray_world(user,color,depth,logical_w,logical_h,projection,remap,rsx_format,radius,strength);
#ifdef PS3RECOMP_RSX_CACAO
    return vk_eng_cacao_world(user,color,depth,logical_w,logical_h,projection,remap,rsx_format,radius,strength);
#else
    return 0;
#endif
}

static const rsx_draw_backend s_vk_engine_backend = {
    .user                 = NULL,
    .init                 = vk_eng_init,
    .shutdown             = vk_eng_shutdown,
    .submit_and_wait      = vk_eng_submit_and_wait,
    .texture_create       = vk_eng_texture_create,
    .texture_upload       = vk_eng_texture_upload,
    .texture_upload_valid = vk_eng_texture_upload_valid,
    .texture_release      = vk_eng_release,
    .color_target_create  = vk_eng_color_target_create,
    .color_target_release = vk_eng_release,
    .surface_view         = vk_eng_surface_view,
    .depth_target_create  = vk_eng_depth_target_create,
    .depth_target_release = vk_eng_release,
    .depth_snapshot       = vk_eng_depth_snapshot,
    .pipeline_create      = vk_eng_pipeline_create,
    .pipeline_release     = vk_eng_pipeline_release,
    .bind_targets         = vk_eng_bind_targets,
    .bind_pipeline        = vk_eng_bind_pipeline,
    .bind_vs_constants    = vk_eng_bind_vs_constants,
    .bind_ps_constants    = vk_eng_bind_ps_constants,
    .bind_textures        = vk_eng_bind_textures,
    .bind_vertex_textures = vk_eng_bind_vertex_textures,
    .set_viewport         = vk_eng_set_viewport,
    .set_scissor          = vk_eng_set_scissor,
    .set_stencil_ref      = vk_eng_set_stencil_ref,
    .draw                 = vk_eng_draw,
    .clear_color          = vk_eng_clear_color,
    .clear_depth_stencil  = vk_eng_clear_depth_stencil,
    .present              = vk_eng_present,
    .set_present_aspect   = vk_eng_set_present_aspect,
    .set_temporal_identity= vk_eng_temporal_identity,
    .readback             = vk_eng_readback,
    .depth_readback       = vk_eng_depth_readback,
    .color_target_write   = vk_eng_color_target_write,
    .color_target_create_multisample = vk_eng_color_multisample,
    .depth_target_create_multisample = vk_eng_depth_multisample,
    .depth_to_argb        = vk_eng_depth_to_argb,
    .ambient_occlusion = vk_eng_world_ao,
};

static rsx_backend s_vulkan_backend = {
    .userdata           = &s_vk,
    .set_render_target  = vk_cb_track_state,
    .set_vertex_attribs = vk_cb_track_state,
    .set_depth_stencil  = vk_cb_track_state,
    .clear              = vk_cb_clear,
    .draw_arrays        = vk_cb_draw_arrays,
    .draw_indexed       = vk_cb_draw_indexed,
    .bind_texture       = vk_cb_bind_texture,
    .present            = vk_cb_present,
};

/* ---------------------------------------------------------------------------
 * Public entry points
 * -------------------------------------------------------------------------*/
#include "rsx_vulkan_advanced.inc"
#include "rsx_vulkan_scene.inc"
#include "rsx_vulkan_scene_batch.inc"
#include "rsx_vulkan_rt_capture.inc"
#include "rsx_vulkan_ray_ao.inc"
#include "rsx_vulkan_dlss.inc"
#include "rsx_vulkan_dlss_frame.inc"

static int vk_init_all(u32 width, u32 height, const char* title)
{
    if (!rsx_vk_anisotropy_parse_override(getenv("PS3RECOMP_ANISOTROPY"), &s_vk.anisotropy_override)) {
        VK_LOG("PS3RECOMP_ANISOTROPY must be auto,0,1,2,4,8 or16\n");
        return -1;
    }
    { const char* mode=getenv("PS3RECOMP_RSX_AO");
      s_vk.rt_requested=mode && !strcmp(mode,"rayquery"); }
    s_vk.width  = width  ? width  : 1280;
    s_vk.height = height ? height : 720;
    if (!rsx_display_extent_valid(s_vk.width,s_vk.height,8192)) {
        VK_LOG("output extent %ux%u exceeds 8192 per axis or 33554432 pixels\n",s_vk.width,s_vk.height);
        return -1;
    }
    s_vk.content_rect=(rsx_display_rect){0,0,s_vk.width,s_vk.height};
    s_vk.dump_path = getenv("PS3RECOMP_VK_DUMP");
    /* Writing a full frame to disk on every present costs more than the
     * present itself and slows a title enough to change what it does; a
     * live title wants one now and then, not all of them. */
    { const char* e = getenv("PS3RECOMP_VK_DUMP_EVERY");
      s_vk.dump_every = (e && atoi(e) > 0) ? (u32)atoi(e) : 1; }

    vk_window_open(title);           /* no-op unless PS3RECOMP_VK_WINDOW is set */
    if (vk_load_library()) return -1;

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "ps3recomp", .pEngineName = "ps3recomp RSX",
                              .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app };
#ifdef PS3RECOMP_RSX_CACAO
    /* Keep the existing Vulkan1.0 path available on older loaders. CACAO's
     * validated shader pack targets1.1 and is disabled when unavailable. */
    PFN_vkEnumerateInstanceVersion enum_version=(PFN_vkEnumerateInstanceVersion)
        pvkGetInstanceProcAddr(VK_NULL_HANDLE,"vkEnumerateInstanceVersion");
    u32 loader_version=VK_API_VERSION_1_0;
    if (enum_version && enum_version(&loader_version)==VK_SUCCESS &&
        loader_version>=VK_API_VERSION_1_1) {
        app.apiVersion=VK_API_VERSION_1_1;s_vk.cacao_api_ok=1;
    }
#endif
    if (s_vk.rt_requested || vk_dlss_requested()) {
        PFN_vkEnumerateInstanceVersion rt_enum=(PFN_vkEnumerateInstanceVersion)
            pvkGetInstanceProcAddr(VK_NULL_HANDLE,"vkEnumerateInstanceVersion");
        u32 version=VK_API_VERSION_1_0;
        if(rt_enum && rt_enum(&version)==VK_SUCCESS && version>=VK_API_VERSION_1_2)
            app.apiVersion=VK_API_VERSION_1_2;
        else VK_LOG("rayquery unavailable: Vulkan1.2 loader required; raster retained\n");
    }
    if (s_vk.windowed) {
        ci.enabledExtensionCount   = s_vk.inst_ext_count;
        ci.ppEnabledExtensionNames = s_vk.inst_exts;
    }
    const char *dlss_inst_names[64];
    vk_dlss_instance_prepare(&ci,dlss_inst_names,64,app.apiVersion);
    VkResult ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    if (ir != VK_SUCCESS && s_vk.windowed) {
        vk_window_drop("vkCreateInstance with the surface extensions failed");
        vk_dlss_shutdown();
        ci.enabledExtensionCount = 0;
        ci.ppEnabledExtensionNames = NULL;
        ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    }
    if (ir != VK_SUCCESS) { VK_LOG("vkCreateInstance failed (VkResult %d)\n", (int)ir); return -1; }
    if (vk_load_instance_functions()) return -1;
    if (s_vk.windowed) {
        if (vk_load_wsi_instance_functions()) {
            vk_window_drop("the driver has no surface functions");
        } else if (!SDL_Vulkan_CreateSurface(s_vk.window, s_vk.instance, &s_vk.surface)) {
            char why[256];
            snprintf(why, sizeof why, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            vk_window_drop(why);
        }
    }
    if (vk_pick_device()) {
        if (!s_vk.windowed) return -1;
        vk_window_drop("no device can present to the window");
        if (vk_pick_device()) return -1;
    }
    if (!rsx_display_extent_valid(s_vk.width,s_vk.height,s_vk.max_dim)) {
        VK_LOG("output extent %ux%u exceeds device limit %u\n",s_vk.width,s_vk.height,s_vk.max_dim);
        return -1;
    }

    vk_advanced_probe(app.apiVersion);

    /* A second queue of the same family for the asynchronous ray transport
     * (no ownership transfers), unless PS3RECOMP_RSX_ASYNC_PT=0. */
    float prio[2] = { 1.0f, 1.0f };
    u32 queue_count = 1;
    {
        const char* async = getenv("PS3RECOMP_RSX_ASYNC_PT");
        u32 families = 0;
        pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys, &families, NULL);
        VkQueueFamilyProperties* fp = families && families <= 64 ?
            (VkQueueFamilyProperties*)calloc(families, sizeof *fp) : NULL;
        if (fp) {
            pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys, &families, fp);
            if (s_vk.rt_requested && !(async && async[0] == '0') && s_vk.queue_family < families &&
                fp[s_vk.queue_family].queueCount >= 2 && (fp[s_vk.queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT))
                queue_count = 2;
            free(fp);
        }
    }
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = s_vk.queue_family,
                                    .queueCount = queue_count, .pQueuePriorities = prio };
    const char* dev_exts[64] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    /* DXT (BC1-3) is how PS3 titles ship most textures; Vulkan gates it
     * behind a feature, enabled only when the device reports it. */
    VkPhysicalDeviceFeatures have, want;
    memset(&want, 0, sizeof want);
    pvkGetPhysicalDeviceFeatures(s_vk.phys, &have);
    want.textureCompressionBC = have.textureCompressionBC;
    s_vk.bc_ok = have.textureCompressionBC ? 1 : 0;
    want.alphaToOne = have.alphaToOne;
    s_vk.alpha_to_one_ok = have.alphaToOne ? 1 : 0;
    want.samplerAnisotropy = have.samplerAnisotropy;
    s_vk.anisotropy_ok = have.samplerAnisotropy ? 1 : 0;
#ifdef PS3RECOMP_RSX_CACAO
    VkPhysicalDeviceProperties cacao_properties;
    pvkGetPhysicalDeviceProperties(s_vk.phys,&cacao_properties);
    u32 cacao_family_count=0;
    pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys,&cacao_family_count,NULL);
    VkQueueFamilyProperties* cacao_families=cacao_family_count && cacao_family_count<=1024
        ? (VkQueueFamilyProperties*)calloc(cacao_family_count,sizeof(*cacao_families)):NULL;
    if (cacao_families) {
        pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys,&cacao_family_count,cacao_families);
        s_vk.cacao_device_ok=s_vk.cacao_api_ok && cacao_properties.apiVersion>=VK_API_VERSION_1_1 &&
            have.shaderStorageImageExtendedFormats && s_vk.queue_family<cacao_family_count &&
            (cacao_families[s_vk.queue_family].queueFlags&VK_QUEUE_COMPUTE_BIT);
        free(cacao_families);
    }
    want.shaderStorageImageExtendedFormats=s_vk.cacao_device_ok ? VK_TRUE : VK_FALSE;
#endif
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = s_vk.windowed ? 1u : 0u,
                               .ppEnabledExtensionNames = s_vk.windowed ? dev_exts : NULL,
                               .pEnabledFeatures = &want };
    VkPhysicalDeviceBufferDeviceAddressFeatures rt_bda={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
        .bufferDeviceAddress=VK_TRUE};
    VkPhysicalDeviceRayQueryFeaturesKHR rt_rq={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        .pNext=&rt_bda,.rayQuery=VK_TRUE};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR rt_as={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext=&rt_rq,.accelerationStructure=VK_TRUE};
    int enable_rt=0;
    if(s_vk.rt_requested && app.apiVersion>=VK_API_VERSION_1_2 &&
       s_advanced.device_api>=VK_API_VERSION_1_2 &&
       s_advanced.acceleration_structure==1 && s_advanced.ray_query==1 &&
       s_advanced.buffer_device_address==1 && s_advanced.deferred_host_operations==1) {
        u32 count=0;pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys,&count,NULL);
        VkQueueFamilyProperties* props=count && count<=1024?
            (VkQueueFamilyProperties*)calloc(count,sizeof *props):NULL;
        if(props) {
            pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys,&count,props);
            enable_rt=s_vk.queue_family<count &&
                (props[s_vk.queue_family].queueFlags&VK_QUEUE_COMPUTE_BIT)!=0;
            free(props);
        }
        VkFormatProperties format;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys,VK_FORMAT_R32G32B32_SFLOAT,&format);
        if(!(format.bufferFeatures&VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR))enable_rt=0;
    }
    if(enable_rt) {
        u32 n=s_vk.windowed?1u:0u;
        dev_exts[n++]=VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
        dev_exts[n++]=VK_KHR_RAY_QUERY_EXTENSION_NAME;
        dev_exts[n++]=VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
        dci.enabledExtensionCount=n;dci.ppEnabledExtensionNames=dev_exts;dci.pNext=&rt_as;
    } else if(s_vk.rt_requested) VK_LOG("rayquery unavailable: required device features/queue/format; raster retained\n");
    VkDeviceCreateInfo baseline_dci=dci;
    VkPhysicalDeviceFeatures baseline_want=want;
    int enable_dlss=vk_dlss_device_prepare(&dci,dev_exts,64);
    if(enable_dlss){
        /* Vulkan12 includes bufferDeviceAddress, so its promoted feature
         * must not also occur in the ray query extension chain. */
        rt_rq.pNext=NULL;
        want.shaderStorageImageExtendedFormats=have.shaderStorageImageExtendedFormats;
        want.shaderInt16=have.shaderInt16;
    }
    VkResult device_result=pvkCreateDevice(s_vk.phys,&dci,NULL,&s_vk.device);
    if(device_result!=VK_SUCCESS && enable_dlss){
        VK_LOG("DLSS device creation failed (%d); retaining prior rendering requirements\n",(int)device_result);
        enable_dlss=0;dci=baseline_dci;want=baseline_want;rt_rq.pNext=&rt_bda;
        vk_dlss_shutdown();
        device_result=pvkCreateDevice(s_vk.phys,&dci,NULL,&s_vk.device);
    }
    if(device_result!=VK_SUCCESS && enable_rt) {
        VK_LOG("rayquery device creation failed (%d); retrying existing raster features\n",(int)device_result);
        enable_rt=0;dci.pNext=NULL;dci.enabledExtensionCount=s_vk.windowed?1u:0u;
        dci.ppEnabledExtensionNames=s_vk.windowed?dev_exts:NULL;
        device_result=pvkCreateDevice(s_vk.phys,&dci,NULL,&s_vk.device);
    }
    VK_CHECK(device_result,"vkCreateDevice");
    s_vk.rt_enabled=enable_rt;
    if(enable_rt)VK_LOG("rayquery logical device features enabled; gameplay scene not yet qualified\n");
    if (vk_load_device_functions()) return -1;
    vk_dlss_device_selected(enable_dlss);
    vk_dlss_device_created();
    vk_pcache_init(s_vk.instance, s_vk.phys, s_vk.device);
    VK_LOG("anisotropy: supported=%d device_limit=%g override=%u (0=guest,1=off)\n",
           s_vk.anisotropy_ok, (double)s_vk.max_anisotropy, s_vk.anisotropy_override);
    if (s_vk.windowed && vk_load_wsi_device_functions())
        vk_window_drop("the driver has no swapchain functions");
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
    memset(&s_async, 0, sizeof s_async);
    if (queue_count == 2) {
        pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 1, &s_async.queue);
        VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (s_async.queue && pvkAllocateCommandBuffers(s_vk.device, &cai, &s_async.cmd) == VK_SUCCESS &&
            pvkCreateFence(s_vk.device, &fci, NULL, &s_async.fence) == VK_SUCCESS &&
            pvkCreateSemaphore(s_vk.device, &sci, NULL, &s_async.ready) == VK_SUCCESS &&
            pvkCreateSemaphore(s_vk.device, &sci, NULL, &s_async.done) == VK_SUCCESS)
            s_async.available = 1;
        VK_LOG("asynchronous ray transport queue: %s\n", s_async.available ? "family queue 1" : "unavailable");
    }

    if (vk_pick_depth_format()) return -1;
    if (vk_create_image(&s_vk.color, VK_FORMAT_R8G8B8A8_UNORM, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | ((s_vk.fxaa_enabled || s_vk.fsr1_enabled) ? VK_IMAGE_USAGE_SAMPLED_BIT : 0), VK_IMAGE_ASPECT_COLOR_BIT)) return -1;
    if (vk_create_image(&s_vk.depth, s_vk.depth_format, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, s_vk.depth_aspect)) return -1;
    if (vk_create_host_buffer(&s_vk.readback, (VkDeviceSize)s_vk.width * s_vk.height * 4u,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return -1;
    if (vk_create_render_pass()) return -1;
    if (vk_create_pipeline_objects()) return -1;
    if (vk_guest_init()) {
        VK_LOG("guest programs: setup failed; refusing fixed-function substitution\n");
        vk_guest_shutdown();
        return -1;
    }

    /* Both targets UNDEFINED -> GENERAL once. Colour starts opaque black so a
     * present before the first clear reads defined memory; depth starts at
     * the FAR plane, not zero -- zero is the near plane, and a guest that
     * enables LESS before its first depth clear would draw nothing (the
     * null backend makes the same choice). */
    if (vk_begin()) return -1;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange cr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange dr = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &cr);
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &dr);
    if (vk_submit_and_wait()) return -1;

    if (s_vk.windowed) {
        VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sem_acquire) != VK_SUCCESS)
            vk_window_drop("the swapchain could not be created");
        else {
            int result = vk_create_swapchain();
            if (result < 0) vk_window_drop("the swapchain could not be created");
            else if (result > 0) s_vk.swapchain_dirty = s_vk.present_deferred = 1;
        }
    }
    return 0;
}

int rsx_vulkan_backend_init(u32 width, u32 height, const char* title)
{
    const char* guest = getenv("PS3RECOMP_VK_GUEST_PROGRAMS");
    if (guest && guest[0] && guest[0] != '0' && !rsx_hlsl_to_spirv_available()) {
        VK_LOG("guest programs requested but HLSL/SPIR-V translator is unavailable\n");
        return -1;
    }
    const char* aa = getenv("PS3RECOMP_ANTIALIASING");
    if (aa && strcmp(aa, "off") && strcmp(aa, "fxaa")) {
        VK_LOG("PS3RECOMP_ANTIALIASING must be off or fxaa\n");
        return -1;
    }
    const char* upscaler = getenv("PS3RECOMP_UPSCALER");
    if (upscaler && strcmp(upscaler, "off") && strcmp(upscaler, "fsr1")) {
        VK_LOG("PS3RECOMP_UPSCALER must be off or fsr1\n");
        return -1;
    }
    memset(&s_vk, 0, sizeof s_vk);
    s_vk.fsr1_enabled = upscaler && !strcmp(upscaler, "fsr1");
    s_vk.fxaa_enabled = aa && !strcmp(aa, "fxaa");
    if (vk_init_all(width, height, title)) {
        rsx_vulkan_backend_shutdown();
        return -1;
    }
    if (s_vk.fxaa_enabled && vk_fxaa_init()) {
        VK_LOG("requested FXAA setup failed\n");
        rsx_vulkan_backend_shutdown();
        return -1;
    }
    VK_LOG("anti-aliasing: %s\n", s_vk.fxaa_enabled ? "FXAA3.11 quality29 RGB-luma" : "off");
    /* One path at a time: the FIFO walker feeds both the rsx_state vtable
     * and the register-file engine, so registering both would record every
     * draw twice (the rule the Metal backend's init follows).
     *
     * With guest programs on, the engine is the default -- as it became
     * Metal's once every host mode passed through it. It builds every
     * pipeline from the guest's programs, so without them it has nothing to
     * draw with, and the vtable's fixed-function path stays the default.
     * Either way PS3RECOMP_RSX_ENGINE=dispatch|vtable overrides. */
    rsx_draw_engine_set_backend(&s_vk_engine_backend);
    rsx_draw_engine_set_default(s_g.on ? 1 : 0);
    if (rsx_draw_engine_enabled() && rsx_draw_engine_init(s_vk.width, s_vk.height) == 0)
        s_vk.eng_active = 1;
    else
        rsx_set_backend(&s_vulkan_backend);
    VK_LOG("%s path\n", s_vk.eng_active ? "register-file draw engine" : "vtable");
    /* Driver compilation can take longer than the FIFO liveness watchdog.
     * Finish requested ray pipelines before the caller releases guest threads. */
    if(s_vk.rt_requested && s_vk.rt_enabled) {
        u64 prepare_start=SDL_GetPerformanceCounter();
        VK_LOG("ray pipeline preparation before guest dispatch started\n");
        if(!vk_ray_initialize()) {
            VK_LOG("requested ray pipeline preparation failed\n");
            rsx_vulkan_backend_shutdown();return -1;
        }
        /* Persist expensive ray compilation before guest dispatch, including
         * runs that never reach their first present. No save thread exists yet. */
        if(s_pcache.cache && SDL_AtomicGet(&s_pcache.dirty)) {
            int saved=vk_pcache_save_now(s_vk.device);
            if(!saved)SDL_AtomicSet(&s_pcache.dirty,0);
            VK_LOG("ray prewarm pipeline cache checkpoint %s\n",saved?"failed":"saved");
        }
        VK_LOG("ray pipeline preparation before guest dispatch completed in %.3fms\n",
            (double)(SDL_GetPerformanceCounter()-prepare_start)*1000.0/(double)SDL_GetPerformanceFrequency());
    }
    if (s_vk.fsr1_enabled)
        VK_LOG("FSR1: %s\n", s_vk.eng_active ? "guest display EASU+RCAS, before optional FXAA" :
               "requested but bypassed on diagnostic vtable path; requires draw engine");
    VK_LOG("offscreen target %ux%u ready, depth format %d (%s)\n",
           s_vk.width, s_vk.height, (int)s_vk.depth_format, title ? title : "");
    s_advanced.initialized = 1;
    VK_LOG("advanced rendering: RT/PT/DLSS unavailable; existing raster settings retained\n");
    return 0;
}

void rsx_vulkan_backend_shutdown(void)
{
    vk_flush_pending(0);
    vhp_close();
    if (rsx_get_backend() == &s_vulkan_backend) rsx_set_backend(NULL);
    if (s_vk.eng_active) { rsx_draw_engine_shutdown(); s_vk.eng_active = 0; }
    rsx_draw_engine_set_backend(NULL);
    if (s_vk.windowed && s_vk.swapchain && s_vk.presented && s_vk.hold_seconds > 0.0) {
        VK_LOG("holding the last frame on screen for %.1f s "
               "(Esc or closing the window ends it early)\n", s_vk.hold_seconds);
        const Uint32 end = SDL_GetTicks() + (Uint32)(s_vk.hold_seconds * 1000.0);
        while (s_vk.windowed && !SDL_TICKS_PASSED(SDL_GetTicks(), end)) {
            if (rsx_vulkan_backend_pump_messages() != 0) break;
            if (vk_prepare_window_present()) vk_present_window();
            else SDL_Delay(10);
        }
    }
    if (s_vk.windowed || s_vk.window) vk_window_drop(NULL);
    if (s_vk.device) {
        if (pvkDeviceWaitIdle) pvkDeviceWaitIdle(s_vk.device);
        vk_dlss_shutdown();
        vk_rt_ao_shutdown();
        vk_rt_capture_shutdown();
        rsx_vulkan_scene_batch_shutdown();
        vk_guest_shutdown();
        for (int i = 0; i < VK_PIPELINE_SLOTS; i++)
            if (s_vk.pipelines[i]) pvkDestroyPipeline(s_vk.device, s_vk.pipelines[i], NULL);
        if (s_vk.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_vk.pipe_layout, NULL);
        if (s_vk.desc_pool)   pvkDestroyDescriptorPool(s_vk.device, s_vk.desc_pool, NULL);
        if (s_vk.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_vk.set_layout, NULL);
        if (s_vk.sampler)     pvkDestroySampler(s_vk.device, s_vk.sampler, NULL);
        for (u32 i = 0; i < s_vk.samp_count; i++)
            pvkDestroySampler(s_vk.device, s_vk.samp_cache[i].s, NULL);
        if (s_vk.vs)          pvkDestroyShaderModule(s_vk.device, s_vk.vs, NULL);
        if (s_vk.fs)          pvkDestroyShaderModule(s_vk.device, s_vk.fs, NULL);
        if (s_vk.framebuffer) pvkDestroyFramebuffer(s_vk.device, s_vk.framebuffer, NULL);
        if (s_vk.render_pass) pvkDestroyRenderPass(s_vk.device, s_vk.render_pass, NULL);
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) vk_destroy_image(&s_vk.tex[u]);
        vk_destroy_image(&s_vk.dummy);
        vk_destroy_image(&s_vk.depth);
        vk_fsr1_shutdown();
        vk_rcas_shutdown();
        vk_fxaa_shutdown();
        vk_destroy_image(&s_vk.color);
        vk_destroy_buffer(&s_vk.vertices);
        vk_destroy_buffer(&s_vk.readback);
        if (s_vk.fence) pvkDestroyFence(s_vk.device, s_vk.fence, NULL);
        if (s_async.fence) pvkDestroyFence(s_vk.device, s_async.fence, NULL);
        if (s_async.ready) pvkDestroySemaphore(s_vk.device, s_async.ready, NULL);
        if (s_async.done) pvkDestroySemaphore(s_vk.device, s_async.done, NULL);
        memset(&s_async, 0, sizeof s_async);
        if (s_vk.pool)  pvkDestroyCommandPool(s_vk.device, s_vk.pool, NULL);
        vk_pcache_shutdown(s_vk.device);
        pvkDestroyDevice(s_vk.device, NULL);
    }
    vk_dlss_shutdown();
    if (s_vk.instance && pvkDestroyInstance) pvkDestroyInstance(s_vk.instance, NULL);
    rsx_vk_library_close(s_vk.lib);
    free(s_vk.cpu_verts);
    memset(&s_vk, 0, sizeof s_vk);
    vk_advanced_reset();
}

int rsx_vulkan_backend_pump_messages(void)
{
    if (!s_vk.windowed) return 0;
    int quit = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        const Uint32 window_id=SDL_GetWindowID(s_vk.window);
        if (e.type==SDL_KEYDOWN && e.key.windowID==window_id &&
            e.key.keysym.sym==SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)) {
            if (!e.key.repeat) {
                const Uint32 mode=(SDL_GetWindowFlags(s_vk.window)&SDL_WINDOW_FULLSCREEN_DESKTOP)
                                  ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP;
                if (mode) vk_place_window_on_display();
                if (SDL_SetWindowFullscreen(s_vk.window,mode)==0) {
                    s_vk.swapchain_dirty=1;
                    VK_LOG("window mode: %s\n",mode?"borderless fullscreen":"windowed");
                } else VK_LOG("fullscreen transition failed: %s\n",SDL_GetError());
            }
            continue; /* Alt+Enter must not also press the guest Start button. */
        }
        if (e.type==SDL_WINDOWEVENT && e.window.windowID==window_id) {
            if (e.window.event==SDL_WINDOWEVENT_SIZE_CHANGED ||
                e.window.event==SDL_WINDOWEVENT_RESTORED ||
                e.window.event==SDL_WINDOWEVENT_MINIMIZED)
                s_vk.swapchain_dirty=1;
            if (e.window.event==SDL_WINDOWEVENT_CLOSE) quit=1;
        }
        ps3_pad_keyboard_sdl_event(&e,SDL_GetWindowID(s_vk.window));
        if (e.type == SDL_QUIT) quit = 1;
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) quit = 1;
    }
    return quit ? -1 : 0;
}

void rsx_vulkan_backend_present(void)
{
    vk_flush_pending(0);
    if (!vk_prepare_window_present()) return;
    if (s_vk.eng_active) rsx_draw_engine_present();
    else                 vk_cb_present(&s_vk, 0);
}

u64 rsx_vulkan_backend_successful_presents(void)
{
    return s_vk.successful_window_presents;
}

u64 rsx_vulkan_backend_successful_offscreen_frames(void)
{
    return s_vk.successful_offscreen_frames;
}

int rsx_vulkan_backend_window_active(void)
{
    return s_vk.windowed && s_vk.surface && s_vk.sem_acquire;
}

int rsx_vulkan_backend_present_deferred(void)
{
    return s_vk.windowed && s_vk.present_deferred;
}

u32 rsx_vulkan_backend_debug_color(void) { return s_vk.clear_argb; }

u32 rsx_vulkan_backend_readback_center(void)
{
    if (s_vk.eng_active) return rsx_draw_engine_readback_center();
    return s_vk.presented ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_guest_draws(void)
{
    return s_vk.eng_active ? rsx_draw_engine_guest_draws() : s_g.last_draws;
}



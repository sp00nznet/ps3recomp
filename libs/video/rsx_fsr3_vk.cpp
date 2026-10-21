#define RSX_FSR3_VK_BUILD
#include "rsx_fsr3_vk.h"
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>

/* ffx_vk_unit.cpp: scratch layout of the unmodified FidelityFX backend. */
extern "C" size_t rsx_ffx_vk_effect_context_offset(size_t max_contexts);
extern "C" size_t rsx_ffx_vk_effect_context_alignment(void);
extern "C" const void *rsx_ffx_vk_effect_contexts(const void *scratch);

struct rsx_fsr3_vk {
    rsx_fsr3_device device{};
    void *scratch_allocation = nullptr;
    void *scratch = nullptr;      /* Placed for the backend's EffectContext array. */
    size_t effect_context_offset = 0;
    FfxInterface iface{};
    FfxFsr3UpscalerContext context{};
    bool context_created = false;
    bool shared_context_created = false;
    FfxUInt32 shared_id = 0;
    FfxResourceInternal shared[3]{};
    uint32_t shared_count = 0;
    bool force_reset = true;
    rsx_fsr3_config config{};
    rsx_fsr3_report report{};
};

namespace {

/* The FSR component copies FfxInterface by value and queries capabilities
 * while its context is created, so the clamp is keyed by the creating
 * thread. The Vulkan backend reports what the device SUPPORTS; only features
 * the caller ENABLED may select shaders: no 16-bit shaders without
 * fp16_enabled, and no forced wave64 (it needs subgroup size control). */
thread_local const rsx_fsr3_vk *t_creating = nullptr;
FfxGetDeviceCapabilitiesFunc g_backend_capabilities = nullptr;

FfxErrorCode clamped_capabilities(FfxInterface *iface, FfxDeviceCapabilities *out)
{
    const FfxErrorCode r = g_backend_capabilities(iface, out);
    if (r != FFX_OK || !out) return r;
    if (!t_creating || !t_creating->device.fp16_enabled) out->fp16Supported = false;
    if (out->maximumSupportedShaderModel > FFX_SHADER_MODEL_6_5)
        out->maximumSupportedShaderModel = FFX_SHADER_MODEL_6_5;
    return r;
}

/* The backend also picks paths from SUPPORTED extensions and then loads their
 * commands by extension name, which is NULL for an extension the caller did
 * not enable (it called vkGetBufferMemoryRequirements2KHR through NULL at
 * creation because VK_KHR_dedicated_allocation was merely supported). Serve
 * the promoted core command instead (Vulkan 1.2 is required) and keep the
 * debug-label and AMD marker hooks absent; the backend checks those. */
PFN_vkGetDeviceProcAddr g_device_proc = nullptr;

PFN_vkVoidFunction VKAPI_CALL bridge_device_proc(VkDevice device, const char *name)
{
    if (!std::strcmp(name, "vkGetBufferMemoryRequirements2KHR")) name = "vkGetBufferMemoryRequirements2";
    else if (!std::strcmp(name, "vkGetImageMemoryRequirements2KHR")) name = "vkGetImageMemoryRequirements2";
    else if (!std::strcmp(name, "vkCmdBeginDebugUtilsLabelEXT") || !std::strcmp(name, "vkCmdEndDebugUtilsLabelEXT") ||
             !std::strcmp(name, "vkSetDebugUtilsObjectNameEXT") || !std::strcmp(name, "vkCmdWriteBufferMarkerAMD") ||
             !std::strcmp(name, "vkCmdWriteBufferMarker2AMD"))
        return nullptr;
    return g_device_proc(device, name);
}

void message(FfxMsgType type, const wchar_t *text)
{
    std::fprintf(stderr, "[fsr3] %s: %ls\n", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning",
                 text ? text : L"");
}

rsx_fsr3_status ffx_status(rsx_fsr3_vk *c, FfxErrorCode r)
{
    c->report.ffx_result = static_cast<int32_t>(r);
    return r == FFX_OK ? RSX_FSR3_OK : RSX_FSR3_FFX_ERROR;
}

bool quality_ok(rsx_fsr3_quality q)
{
    return q >= RSX_FSR3_NATIVE_AA && q <= RSX_FSR3_ULTRA_PERFORMANCE;
}

bool format_in(VkFormat f, std::initializer_list<VkFormat> allowed)
{
    for (VkFormat a : allowed) if (f == a) return true;
    return false;
}

/* Inputs are read by texel fetch inside the render extent, so they may be
 * larger than it (a max-size target). The output must match exactly. */
bool image_ok(const rsx_fsr3_image &i, std::initializer_list<VkFormat> formats,
              uint32_t min_w, uint32_t min_h, bool exact, VkImageLayout layout,
              VkImageUsageFlags needed)
{
    if (i.image == VK_NULL_HANDLE || !format_in(i.format, formats) ||
        i.declared_layout != layout || (i.declared_usage & needed) != needed)
        return false;
    return exact ? (i.width == min_w && i.height == min_h)
                 : (i.width >= min_w && i.height >= min_h);
}

FfxResource ffx_image(const rsx_fsr3_image &i, const wchar_t *name, FfxResourceStates state)
{
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = i.format;
    ci.extent = {i.width, i.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = i.declared_usage;
    return ffxGetResourceVK(reinterpret_cast<void *>(i.image),
                            ffxGetImageResourceDescriptionVK(i.image, ci, FFX_RESOURCE_USAGE_READ_ONLY),
                            name, state);
}

void release_configuration(rsx_fsr3_vk *c)
{
    for (uint32_t n = 0; n < c->shared_count; ++n)
        c->iface.fpDestroyResource(&c->iface, c->shared[n], c->shared_id);
    c->shared_count = 0;
    if (c->shared_context_created) c->iface.fpDestroyBackendContext(&c->iface, c->shared_id);
    c->shared_context_created = false;
    if (c->context_created) ffxFsr3UpscalerContextDestroy(&c->context);
    c->context_created = false;
    c->report.configured = 0;
}

} // namespace

extern "C" RSX_FSR3_API rsx_fsr3_status rsx_fsr3_device_requirements(
    VkPhysicalDevice physical, rsx_fsr3_requirements *out)
{
    if (physical == VK_NULL_HANDLE || !out) return RSX_FSR3_BAD_ARGUMENT;
    std::memset(out, 0, sizeof *out);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    VkPhysicalDeviceSubgroupProperties subgroup{};
    subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &subgroup;
    VkPhysicalDeviceVulkan12Features v12{};
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &v12;
    if (properties.apiVersion < VK_API_VERSION_1_2) return RSX_FSR3_UNAVAILABLE;
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    vkGetPhysicalDeviceFeatures2(physical, &features2);
    const VkPhysicalDeviceFeatures &have = features2.features;
    /* From the FSR 3.1 SPIR-V: StorageImageExtendedFormats,
     * StorageImageWriteWithoutFormat, GroupNonUniform(Quad) in compute. */
    const VkSubgroupFeatureFlags ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
    if (!have.shaderStorageImageExtendedFormats || !have.shaderStorageImageWriteWithoutFormat ||
        !(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
        (subgroup.supportedOperations & ops) != ops)
        return RSX_FSR3_UNAVAILABLE;
    out->required.shaderStorageImageExtendedFormats = VK_TRUE;
    out->required.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    out->fp16_supported = v12.shaderFloat16 && have.shaderInt16;
    return RSX_FSR3_OK;
}

extern "C" RSX_FSR3_API rsx_fsr3_status rsx_fsr3_create(
    const rsx_fsr3_device *device, rsx_fsr3_vk **out, rsx_fsr3_report *report)
{
    if (out) *out = nullptr;
    if (report) std::memset(report, 0, sizeof *report);
    if (!device || !out || device->physical == VK_NULL_HANDLE || device->device == VK_NULL_HANDLE ||
        !device->get_device_proc || device->fp16_enabled > 1)
        return RSX_FSR3_BAD_ARGUMENT;
    rsx_fsr3_requirements requirements{};
    if (rsx_fsr3_device_requirements(device->physical, &requirements) != RSX_FSR3_OK)
        return RSX_FSR3_UNAVAILABLE;
    if (device->fp16_enabled && !requirements.fp16_supported) return RSX_FSR3_BAD_ARGUMENT;
    if (g_device_proc && g_device_proc != device->get_device_proc) return RSX_FSR3_BAD_ARGUMENT;
    rsx_fsr3_vk *c = new (std::nothrow) rsx_fsr3_vk;
    if (!c) return RSX_FSR3_NO_MEMORY;
    c->device = *device;
    /* One FSR upscaler context plus the shared-resource context. */
    const size_t contexts = FFX_FSR3UPSCALER_CONTEXT_COUNT + 1;
    const size_t size = ffxGetScratchMemorySizeVK(device->physical, contexts);
    const size_t align = rsx_ffx_vk_effect_context_alignment();
    c->effect_context_offset = rsx_ffx_vk_effect_context_offset(contexts);
    if (!size || c->effect_context_offset % 8 || align < 8 || align & (align - 1)) { delete c; return RSX_FSR3_BAD_STATE; }
    c->scratch_allocation = std::calloc(1, size + align);
    if (!c->scratch_allocation) { delete c; return RSX_FSR3_NO_MEMORY; }
    /* Base 8-aligned (the backend's own structures) with base + offset
     * aligned for the alignas(32) EffectContext array. */
    uintptr_t base = (reinterpret_cast<uintptr_t>(c->scratch_allocation) + 7) & ~uintptr_t(7);
    while ((base + c->effect_context_offset) % align) base += 8;
    c->scratch = reinterpret_cast<void *>(base);
    g_device_proc = device->get_device_proc;
    VkDeviceContext vk{device->device, device->physical, bridge_device_proc};
    const FfxErrorCode r = ffxGetInterfaceVK(&c->iface, ffxGetDeviceVK(&vk), c->scratch, size, contexts);
    if (r != FFX_OK) {
        if (report) report->ffx_result = r;
        std::free(c->scratch_allocation); delete c;
        return RSX_FSR3_FFX_ERROR;
    }
    g_backend_capabilities = c->iface.fpGetDeviceCapabilities;
    c->iface.fpGetDeviceCapabilities = clamped_capabilities;
    c->report.fp16_shaders = device->fp16_enabled;
    if (report) *report = c->report;
    *out = c;
    return RSX_FSR3_OK;
}

extern "C" RSX_FSR3_API void rsx_fsr3_destroy(rsx_fsr3_vk *c)
{
    if (!c) return;
    release_configuration(c);
    std::free(c->scratch_allocation);
    delete c;
}

extern "C" RSX_FSR3_API rsx_fsr3_status rsx_fsr3_get_report(
    const rsx_fsr3_vk *c, rsx_fsr3_report *out)
{
    if (!c || !out) return RSX_FSR3_BAD_ARGUMENT;
    *out = c->report;
    return RSX_FSR3_OK;
}

extern "C" RSX_FSR3_API rsx_fsr3_status rsx_fsr3_render_size(
    uint32_t output_width, uint32_t output_height, rsx_fsr3_quality quality,
    uint32_t *render_width, uint32_t *render_height)
{
    if (!output_width || !output_height || !quality_ok(quality) || !render_width || !render_height)
        return RSX_FSR3_BAD_ARGUMENT;
    return ffxFsr3UpscalerGetRenderResolutionFromQualityMode(
               render_width, render_height, output_width, output_height,
               static_cast<FfxFsr3UpscalerQualityMode>(quality)) == FFX_OK
               ? RSX_FSR3_OK : RSX_FSR3_BAD_ARGUMENT;
}

extern "C" RSX_FSR3_API rsx_fsr3_status rsx_fsr3_jitter(
    uint32_t render_width, uint32_t output_width, uint64_t frame_index,
    float *jitter_x, float *jitter_y, uint32_t *phase_count)
{
    if (!render_width || render_width > output_width || !jitter_x || !jitter_y)
        return RSX_FSR3_BAD_ARGUMENT;
    const int32_t phases = ffxFsr3UpscalerGetJitterPhaseCount(
        static_cast<int32_t>(render_width), static_cast<int32_t>(output_width));
    if (phases <= 0) return RSX_FSR3_BAD_ARGUMENT;
    if (phase_count) *phase_count = static_cast<uint32_t>(phases);
    return ffxFsr3UpscalerGetJitterOffset(jitter_x, jitter_y,
               static_cast<int32_t>(frame_index % static_cast<uint64_t>(phases)), phases) == FFX_OK
               ? RSX_FSR3_OK : RSX_FSR3_BAD_ARGUMENT;
}

extern "C" RSX_FSR3_API rsx_fsr3_status rsx_fsr3_configure(
    rsx_fsr3_vk *c, const rsx_fsr3_config *config)
{
    if (!c || c->report.configured || c->context_created) return RSX_FSR3_BAD_STATE;
    if (!config || !config->generation || !config->max_render_width || !config->max_render_height ||
        config->max_render_width > config->output_width || config->max_render_height > config->output_height ||
        config->color > RSX_FSR3_COLOR_LINEAR_HDR || config->depth_inverted > 1 ||
        config->depth_infinite > 1 || config->auto_exposure > 1)
        return RSX_FSR3_BAD_ARGUMENT;
    FfxFsr3UpscalerContextDescription d{};
    d.flags = (config->color == RSX_FSR3_COLOR_LINEAR_HDR ? FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE : 0) |
              (config->depth_inverted ? FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED : 0) |
              (config->depth_infinite ? FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE : 0) |
              (config->auto_exposure ? FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE : 0) |
              (std::getenv("RSX_FSR3_DEBUG") ? FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING : 0);
    d.maxRenderSize = {config->max_render_width, config->max_render_height};
    d.maxUpscaleSize = {config->output_width, config->output_height};
    d.fpMessage = message;
    d.backendInterface = c->iface;
    t_creating = c;
    FfxErrorCode r = ffxFsr3UpscalerContextCreate(&c->context, &d);
    t_creating = nullptr;
    if (ffx_status(c, r) != RSX_FSR3_OK) return RSX_FSR3_FFX_ERROR;
    c->context_created = true;
    if (rsx_ffx_vk_effect_contexts(c->scratch) !=
        static_cast<const uint8_t *>(c->scratch) + c->effect_context_offset) {
        std::fprintf(stderr, "[fsr3] FidelityFX scratch layout differs from ffx_vk_unit.cpp\n");
        release_configuration(c);
        return RSX_FSR3_BAD_STATE;
    }
    /* FSR 3.1 writes these between passes; a null resource would bind the
     * backend's NULL slot. Allocated as the SDK's own FSR3 component does. */
    r = c->iface.fpCreateBackendContext(&c->iface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &c->shared_id);
    if (r == FFX_OK) {
        c->shared_context_created = true;
        FfxFsr3UpscalerSharedResourceDescriptions s{};
        r = ffxFsr3UpscalerGetSharedResourceDescriptions(&c->context, &s);
        const FfxCreateResourceDescription *descs[3] = {&s.dilatedDepth, &s.dilatedMotionVectors,
                                                        &s.reconstructedPrevNearestDepth};
        for (uint32_t n = 0; r == FFX_OK && n < 3; ++n) {
            r = c->iface.fpCreateResource(&c->iface, descs[n], c->shared_id, &c->shared[n]);
            if (r == FFX_OK) c->shared_count = n + 1;
        }
    }
    if (ffx_status(c, r) != RSX_FSR3_OK) { release_configuration(c); return RSX_FSR3_FFX_ERROR; }
    FfxEffectMemoryUsage usage{};
    if (ffxFsr3UpscalerContextGetGpuMemoryUsage(&c->context, &usage) == FFX_OK)
        c->report.gpu_memory_bytes = usage.totalUsageInBytes;
    c->config = *config;
    c->report.configured = 1;
    c->force_reset = true;
    return RSX_FSR3_OK;
}

extern "C" RSX_FSR3_API rsx_fsr3_status rsx_fsr3_evaluate(
    rsx_fsr3_vk *c, VkCommandBuffer command, const rsx_fsr3_frame *f)
{
    if (!c || !f || command == VK_NULL_HANDLE || !c->report.configured) return RSX_FSR3_BAD_STATE;
    c->report.last_evaluation_succeeded = 0;
    const rsx_fsr3_config &cfg = c->config;
    const VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const uint32_t rw = f->render_width, rh = f->render_height;
    if (f->config_generation != cfg.generation || !f->frame_serial ||
        f->motion_frame_serial != f->frame_serial ||
        f->motion_provenance != RSX_FSR3_MOTION_CAMERA_AND_OBJECT_UNJITTERED || f->reset > 1 ||
        !rw || !rh || rw > cfg.max_render_width || rh > cfg.max_render_height ||
        !std::isfinite(f->jitter_x) || !std::isfinite(f->jitter_y) ||
        std::fabs(f->jitter_x) > 1.0f || std::fabs(f->jitter_y) > 1.0f ||
        !std::isfinite(f->delta_milliseconds) || f->delta_milliseconds <= 0 ||
        !std::isfinite(f->pre_exposure) || f->pre_exposure <= 0 ||
        !std::isfinite(f->camera_near) || f->camera_near <= 0 ||
        !(f->camera_far > f->camera_near) ||
        !std::isfinite(f->vertical_fov_radians) || f->vertical_fov_radians <= 0 ||
        f->vertical_fov_radians >= 3.14159265f ||
        !std::isfinite(f->sharpness) || f->sharpness < 0 || f->sharpness > 1 ||
        !image_ok(f->color, {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM}, rw, rh, false, read,
                  VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !image_ok(f->depth, {VK_FORMAT_R32_SFLOAT}, rw, rh, false, read, VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !image_ok(f->motion, {VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R16G16_SFLOAT}, rw, rh, false, read,
                  VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !image_ok(f->output, {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM},
                  cfg.output_width, cfg.output_height, true, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_USAGE_STORAGE_BIT))
        return RSX_FSR3_BAD_ARGUMENT;
    if (f->reactive.image &&
        !image_ok(f->reactive, {VK_FORMAT_R8_UNORM}, rw, rh, false, read, VK_IMAGE_USAGE_SAMPLED_BIT))
        return RSX_FSR3_BAD_ARGUMENT;
    if (f->output.image == f->color.image || f->output.image == f->depth.image ||
        f->output.image == f->motion.image || (f->reactive.image && f->output.image == f->reactive.image))
        return RSX_FSR3_BAD_ARGUMENT;
    if (c->report.evaluation_count && f->frame_serial <= c->report.last_frame_serial)
        return RSX_FSR3_BAD_ARGUMENT;
    const bool consecutive = c->report.evaluation_count && c->report.last_frame_serial != UINT64_MAX &&
                             f->frame_serial == c->report.last_frame_serial + 1;
    if ((!consecutive || c->force_reset) && !f->reset) return RSX_FSR3_BAD_ARGUMENT;

    FfxFsr3UpscalerDispatchDescription d{};
    d.commandList = ffxGetCommandListVK(command);
    d.color = ffx_image(f->color, L"FSR3_InputColor", FFX_RESOURCE_STATE_COMPUTE_READ);
    d.depth = ffx_image(f->depth, L"FSR3_InputDepth", FFX_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffx_image(f->motion, L"FSR3_InputMotionVectors", FFX_RESOURCE_STATE_COMPUTE_READ);
    if (f->reactive.image)
        d.reactive = ffx_image(f->reactive, L"FSR3_InputReactiveMap", FFX_RESOURCE_STATE_COMPUTE_READ);
    d.output = ffx_image(f->output, L"FSR3_OutputUpscaledColor", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.dilatedDepth = c->iface.fpGetResource(&c->iface, c->shared[0]);
    d.dilatedMotionVectors = c->iface.fpGetResource(&c->iface, c->shared[1]);
    d.reconstructedPrevNearestDepth = c->iface.fpGetResource(&c->iface, c->shared[2]);
    d.jitterOffset = {f->jitter_x, f->jitter_y};
    d.motionVectorScale = {1.0f, 1.0f};  /* render pixels already */
    d.renderSize = {rw, rh};
    d.upscaleSize = {cfg.output_width, cfg.output_height};
    d.enableSharpening = f->sharpness > 0;
    d.sharpness = f->sharpness;
    d.frameTimeDelta = f->delta_milliseconds;
    d.preExposure = f->pre_exposure;
    d.reset = f->reset != 0;
    d.cameraNear = f->camera_near;
    d.cameraFar = f->camera_far;
    d.cameraFovAngleVertical = f->vertical_fov_radians;
    d.viewSpaceToMetersFactor = 1.0f;
    if (ffx_status(c, ffxFsr3UpscalerContextDispatch(&c->context, &d)) != RSX_FSR3_OK) {
        c->force_reset = true;
        return RSX_FSR3_FFX_ERROR;
    }
    c->force_reset = false;
    c->report.last_evaluation_succeeded = 1;
    ++c->report.evaluation_count;
    c->report.last_frame_serial = f->frame_serial;
    return RSX_FSR3_OK;
}

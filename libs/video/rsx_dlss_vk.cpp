#define NOMINMAX
#define RSX_DLSS_VK_BUILD
#include "rsx_dlss_vk.h"
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_defs_dlssg.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
static_assert(sizeof(wchar_t) == sizeof(uint16_t), "Windows UTF-16 ABI required");
static_assert(sizeof(void *) == 8, "64-bit Vulkan/NGX ABI required");
struct rsx_dlss_vk {
    rsx_dlss_device device{};
    std::string project;
    std::wstring log_path, runtime_path;
    const wchar_t *runtime_paths[1]{};
    NVSDK_NGX_FeatureCommonInfo common{};
    NVSDK_NGX_Parameter *parameters = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;
    NVSDK_NGX_Parameter *fg_parameters = nullptr;
    NVSDK_NGX_Handle *fg_feature = nullptr;
    bool initialized = false;
    bool force_reset = true;
    bool creation_failed = false;
    rsx_dlss_config config{};
    rsx_dlss_report report{};
    rsx_dlss_fg_config fg_config{};
    rsx_dlss_fg_report fg_report{};
    bool fg_creation_failed = false;
    bool fg_force_reset = true;
};
namespace {
static void NVSDK_CONV diagnostic_log(const char *message,NVSDK_NGX_Logging_Level level,NVSDK_NGX_Feature feature)
{
    if(message)std::fprintf(stderr,"[DLSS NGX level=%d feature=%d] %.4096s\n",(int)level,(int)feature,message);
}
static void diagnostic_logging(NVSDK_NGX_FeatureCommonInfo &common)
{
    const char *trace=std::getenv("PS3RECOMP_DLSS_TRACE");
    if(trace && !std::strcmp(trace,"1")){
        common.LoggingInfo.LoggingCallback=diagnostic_log;
        common.LoggingInfo.MinimumLoggingLevel=NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    }
}
static bool uuid_ok(const char *s)
{
    if (!s) return false;
    for (unsigned i = 0; i < 36; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !((c >= '0' && c <= '9') ||
                                (c >= 'a' && c <= 'f') ||
                                (c >= 'A' && c <= 'F'))) return false;
    }
    return s[36] == '\0';
}
static bool absolute_windows_path(const std::wstring &s)
{
    if (s.size() < 3 || s.size() > 32700) return false;
    bool drive = ((s[0] >= L'A' && s[0] <= L'Z') ||
                  (s[0] >= L'a' && s[0] <= L'z')) &&
                 s[1] == L':' && (s[2] == L'\\' || s[2] == L'/');
    bool unc = s.size() >= 5 && s[0] == L'\\' && s[1] == L'\\' &&
               s[2] != L'\\';
    return drive || unc;
}
static bool existing_directory(const std::wstring &path)
{
    DWORD flags = GetFileAttributesW(path.c_str());
    return flags != INVALID_FILE_ATTRIBUTES &&
           (flags & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
static bool named_runtime_exists(const std::wstring &directory,
                                 const wchar_t *name)
{
    if (directory.empty() || directory.size() > 32680) return false;
    try {
        std::wstring file = directory;
        if (file.back() != L'\\' && file.back() != L'/') file += L'\\';
        file += name;
        DWORD flags = GetFileAttributesW(file.c_str());
        return flags != INVALID_FILE_ATTRIBUTES &&
               !(flags & FILE_ATTRIBUTE_DIRECTORY);
    } catch (...) { return false; }
}
/* -1: not loaded yet; 0: wrong path; 1: requested runtime loaded. */
static int expected_runtime_loaded(const std::wstring &directory,
                                   const wchar_t *name)
{
    HMODULE module = GetModuleHandleW(name);
    if (!module) return -1;
    try {
        wchar_t loaded[32768], loaded_full[32768], expected_full[32768];
        DWORD n = GetModuleFileNameW(module, loaded, 32768);
        if (!n || n >= 32768) return 0;
        std::wstring expected = directory;
        if (expected.back() != L'\\' && expected.back() != L'/') expected += L'\\';
        expected += name;
        DWORD a = GetFullPathNameW(loaded, 32768, loaded_full, nullptr);
        DWORD b = GetFullPathNameW(expected.c_str(), 32768, expected_full, nullptr);
        return a && b && a < 32768 && b < 32768 &&
               CompareStringOrdinal(loaded_full, -1, expected_full, -1, TRUE) ==
                   CSTR_EQUAL;
    } catch (...) { return 0; }
}
static bool copy_utf16_path(const uint16_t *source, std::wstring &out)
{
    if (!source) return false;
    for (unsigned i = 0; i <= 32700; ++i) {
        if (!source[i]) return !out.empty();
        out.push_back(static_cast<wchar_t>(source[i]));
    }
    return false;
}
struct Discovery {
    std::string project;
    std::wstring log_path, runtime_path;
    const wchar_t *paths[1]{};
    NVSDK_NGX_FeatureCommonInfo common{};
    NVSDK_NGX_FeatureDiscoveryInfo info{};
    bool fill(const rsx_dlss_identity *source,
              NVSDK_NGX_Feature feature = NVSDK_NGX_Feature_SuperSampling)
    {
        if (!source || !uuid_ok(source->project_uuid) ||
            !source->log_directory_utf16 || !source->runtime_directory_utf16)
            return false;
        project = source->project_uuid;
        if (!copy_utf16_path(source->log_directory_utf16, log_path) ||
            !copy_utf16_path(source->runtime_directory_utf16, runtime_path))
            return false;
        if (!absolute_windows_path(log_path) ||
            !absolute_windows_path(runtime_path) ||
            !existing_directory(log_path) || !existing_directory(runtime_path) ||
            runtime_path.size() > 32680) return false;
        if (!named_runtime_exists(runtime_path,
            feature == NVSDK_NGX_Feature_FrameGeneration ?
                L"nvngx_dlssg.dll" : L"nvngx_dlss.dll"))
            return false;
        paths[0] = runtime_path.c_str();
        common.PathListInfo.Path = paths;
        common.PathListInfo.Length = 1;
        diagnostic_logging(common);
        info.SDKVersion = NVSDK_NGX_Version_API;
        info.FeatureID = feature;
        info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
        info.Identifier.v.ProjectDesc = {project.c_str(),
                                          NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                          "folklore-native-vulkan"};
        info.ApplicationDataPath = log_path.c_str();
        info.FeatureInfo = &common;
        return true;
    }
};
static rsx_dlss_status copy_extensions(uint32_t count,
                                       const VkExtensionProperties *input,
                                       rsx_dlss_extensions *out)
{
    if (count > RSX_DLSS_MAX_EXTENSIONS || (count && !input))
        return RSX_DLSS_UNAVAILABLE;
    for (uint32_t i = 0; i < count; ++i) {
        if (!std::memchr(input[i].extensionName, 0, VK_MAX_EXTENSION_NAME_SIZE))
            return RSX_DLSS_UNAVAILABLE;
        out->items[i] = input[i];
    }
    out->count = count;
    return RSX_DLSS_OK;
}
static bool quality_ok(rsx_dlss_quality quality)
{
    return quality >= RSX_DLSS_PERFORMANCE && quality <= RSX_DLSS_DLAA;
}
static bool image_ok(const rsx_dlss_image &i, VkFormat format,
                     uint32_t width, uint32_t height, VkImageLayout layout)
{
    const VkImageSubresourceRange &s = i.subresource;
    return i.image != VK_NULL_HANDLE && i.view != VK_NULL_HANDLE &&
           i.format == format && i.width == width && i.height == height &&
           s.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && s.baseMipLevel == 0 &&
           s.levelCount == 1 && s.baseArrayLayer == 0 && s.layerCount == 1 &&
           i.declared_layout == layout;
}

static NVSDK_NGX_Resource_VK ngx_image(const rsx_dlss_image &i, bool write)
{
    NVSDK_NGX_Resource_VK r{};
    r.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    r.ReadWrite = write;
    r.Resource.ImageViewInfo = {i.view, i.image, i.subresource,
                                i.format, i.width, i.height};
    return r;
}

static rsx_dlss_status ngx_status(rsx_dlss_vk *c, NVSDK_NGX_Result r)
{
    c->report.ngx_result = static_cast<int32_t>(r);
    return NVSDK_NGX_FAILED(r) ? RSX_DLSS_NGX_ERROR : RSX_DLSS_OK;
}

static void release_context(rsx_dlss_vk *c)
{
    if (!c) return;
    if (c->fg_feature) NVSDK_NGX_VULKAN_ReleaseFeature(c->fg_feature);
    if (c->fg_parameters) NVSDK_NGX_VULKAN_DestroyParameters(c->fg_parameters);
    if (c->feature) NVSDK_NGX_VULKAN_ReleaseFeature(c->feature);
    if (c->parameters) NVSDK_NGX_VULKAN_DestroyParameters(c->parameters);
    if (c->initialized) NVSDK_NGX_VULKAN_Shutdown1(c->device.device);
    delete c;
}

} // namespace

extern "C" RSX_DLSS_API rsx_dlss_status rsx_dlss_instance_extensions(
    const rsx_dlss_identity *identity, rsx_dlss_extensions *out,
    int32_t *ngx_result)
{
    if (!out) return RSX_DLSS_BAD_ARGUMENT;
    out->count = 0;
    if (ngx_result) *ngx_result = 0;
    try {
        Discovery d;
        if (!d.fill(identity)) return RSX_DLSS_BAD_ARGUMENT;
        uint32_t count = 0;
        VkExtensionProperties *extensions = nullptr;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
            &d.info, &count, &extensions);
        if (ngx_result) *ngx_result = static_cast<int32_t>(r);
        if (NVSDK_NGX_FAILED(r)) return RSX_DLSS_NGX_ERROR;
        return copy_extensions(count, extensions, out);
    } catch (const std::bad_alloc &) { return RSX_DLSS_NO_MEMORY; }
    catch (...) { return RSX_DLSS_BAD_ARGUMENT; }
}

extern "C" RSX_DLSS_API rsx_dlss_status rsx_dlss_device_extensions(
    const rsx_dlss_identity *identity, VkInstance instance,
    VkPhysicalDevice physical, rsx_dlss_extensions *out,
    int32_t *ngx_result)
{
    if (!out || instance == VK_NULL_HANDLE || physical == VK_NULL_HANDLE)
        return RSX_DLSS_BAD_ARGUMENT;
    out->count = 0;
    if (ngx_result) *ngx_result = 0;
    try {
        Discovery d;
        if (!d.fill(identity)) return RSX_DLSS_BAD_ARGUMENT;
        uint32_t count = 0;
        VkExtensionProperties *extensions = nullptr;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
            instance, physical, &d.info, &count, &extensions);
        if (ngx_result) *ngx_result = static_cast<int32_t>(r);
        if (NVSDK_NGX_FAILED(r)) return RSX_DLSS_NGX_ERROR;
        return copy_extensions(count, extensions, out);
    } catch (const std::bad_alloc &) { return RSX_DLSS_NO_MEMORY; }
    catch (...) { return RSX_DLSS_BAD_ARGUMENT; }
}

extern "C" RSX_DLSS_API rsx_dlss_status rsx_dlss_create(
    const rsx_dlss_identity *identity, const rsx_dlss_device *device,
    rsx_dlss_vk **out, rsx_dlss_report *report)
{
    if (!out || !device || device->instance == VK_NULL_HANDLE ||
        device->physical == VK_NULL_HANDLE || device->device == VK_NULL_HANDLE ||
        !device->get_instance_proc || !device->get_device_proc)
        return RSX_DLSS_BAD_ARGUMENT;
    *out = nullptr;
    if (report) *report = {};
    try {
        Discovery d;
        if (!d.fill(identity)) return RSX_DLSS_BAD_ARGUMENT;
        std::unique_ptr<rsx_dlss_vk, void(*)(rsx_dlss_vk *)> c(
            new (std::nothrow) rsx_dlss_vk, release_context);
        if (!c) return RSX_DLSS_NO_MEMORY;
        c->device = *device;
        c->project = d.project;
        c->log_path = d.log_path;
        c->runtime_path = d.runtime_path;
        c->runtime_paths[0] = c->runtime_path.c_str();
        c->common.PathListInfo.Path = c->runtime_paths;
        c->common.PathListInfo.Length = 1;
        diagnostic_logging(c->common);
        NVSDK_NGX_FeatureRequirement requirement{};
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureRequirements(
            device->instance, device->physical, &d.info, &requirement);
        rsx_dlss_status status = ngx_status(c.get(), r);
        if (status != RSX_DLSS_OK) {
            if (report) *report = c->report;
            return status;
        }
        c->report.support_flags = static_cast<uint32_t>(requirement.FeatureSupported);
        if (c->report.support_flags) {
            if (report) *report = c->report;
            return RSX_DLSS_UNAVAILABLE;
        }
        r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
            c->project.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            "folklore-native-vulkan", c->log_path.c_str(),
            device->instance, device->physical, device->device,
            device->get_instance_proc, device->get_device_proc, &c->common);
        status = ngx_status(c.get(), r);
        if (status != RSX_DLSS_OK) {
            if (report) *report = c->report;
            return status;
        }
        c->initialized = true;
        r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&c->parameters);
        status = ngx_status(c.get(), r);
        if (status != RSX_DLSS_OK || !c->parameters) {
            if (report) *report = c->report;
            return status == RSX_DLSS_OK ? RSX_DLSS_UNAVAILABLE : status;
        }
        int available = 0, needs_driver = 0;
        r = c->parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available,
                               &available);
        status = ngx_status(c.get(), r);
        if (status == RSX_DLSS_OK) {
            r = c->parameters->Get(
                NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                &needs_driver);
            status = ngx_status(c.get(), r);
        }
        c->report.available = available != 0;
        c->report.needs_driver_update = needs_driver != 0;
        if (report) *report = c->report;
        if (status != RSX_DLSS_OK || !available || needs_driver) {
            return status == RSX_DLSS_OK ? RSX_DLSS_UNAVAILABLE : status;
        }
        *out = c.release();
        return RSX_DLSS_OK;
    } catch (const std::bad_alloc &) { return RSX_DLSS_NO_MEMORY; }
    catch (...) { return RSX_DLSS_BAD_ARGUMENT; }
}

extern "C" RSX_DLSS_API void rsx_dlss_destroy(rsx_dlss_vk *context)
{
    release_context(context);
}

extern "C" RSX_DLSS_API rsx_dlss_status rsx_dlss_get_report(
    const rsx_dlss_vk *context, rsx_dlss_report *out)
{
    if (!context || !out) return RSX_DLSS_BAD_ARGUMENT;
    *out = context->report;
    return RSX_DLSS_OK;
}

extern "C" RSX_DLSS_API rsx_dlss_status rsx_dlss_optimal_size(
    rsx_dlss_vk *c, uint32_t width, uint32_t height,
    rsx_dlss_quality quality, rsx_dlss_size *out)
{
    if (!c || !out || !width || !height || !quality_ok(quality) ||
        c->report.configured) return RSX_DLSS_BAD_ARGUMENT;
    void *callback = nullptr;
    NVSDK_NGX_Result r = c->parameters->Get(
        NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback);
    if (ngx_status(c, r) != RSX_DLSS_OK) return RSX_DLSS_NGX_ERROR;
    if (!callback) return RSX_DLSS_UNAVAILABLE;
    c->parameters->Set(NVSDK_NGX_Parameter_Width, width);
    c->parameters->Set(NVSDK_NGX_Parameter_Height, height);
    c->parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                       static_cast<int>(quality));
    c->parameters->Set(NVSDK_NGX_Parameter_RTXValue, 0);
    r = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(
        callback)(c->parameters);
    if (ngx_status(c, r) != RSX_DLSS_OK) return RSX_DLSS_NGX_ERROR;
    rsx_dlss_size value{};
    r = c->parameters->Get(NVSDK_NGX_Parameter_OutWidth, &value.input_width);
    if (ngx_status(c, r) != RSX_DLSS_OK) return RSX_DLSS_NGX_ERROR;
    r = c->parameters->Get(NVSDK_NGX_Parameter_OutHeight, &value.input_height);
    if (ngx_status(c, r) != RSX_DLSS_OK) return RSX_DLSS_NGX_ERROR;
    if (!value.input_width || !value.input_height ||
        value.input_width > width || value.input_height > height)
        return RSX_DLSS_UNAVAILABLE;
    value.minimum_width = value.maximum_width = value.input_width;
    value.minimum_height = value.maximum_height = value.input_height;
    c->parameters->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width,
                       &value.minimum_width);
    c->parameters->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height,
                       &value.minimum_height);
    c->parameters->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width,
                       &value.maximum_width);
    c->parameters->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height,
                       &value.maximum_height);
    *out = value;
    return RSX_DLSS_OK;
}

extern "C" RSX_DLSS_API rsx_dlss_status rsx_dlss_configure(
    rsx_dlss_vk *c, VkCommandBuffer command, const rsx_dlss_config *cfg)
{
    if (!c || !cfg || command == VK_NULL_HANDLE || c->report.configured ||
        c->creation_failed ||
        !cfg->generation || !cfg->input_width || !cfg->input_height ||
        !cfg->output_width || !cfg->output_height ||
        cfg->input_width > cfg->output_width ||
        cfg->input_height > cfg->output_height || !quality_ok(cfg->quality) ||
        cfg->auto_exposure > 1) return RSX_DLSS_BAD_ARGUMENT;
    rsx_dlss_size optimal{};
    rsx_dlss_status status = rsx_dlss_optimal_size(
        c, cfg->output_width, cfg->output_height, cfg->quality, &optimal);
    if (status != RSX_DLSS_OK) return status;
    if (cfg->input_width != optimal.input_width ||
        cfg->input_height != optimal.input_height) return RSX_DLSS_BAD_ARGUMENT;
    NVSDK_NGX_Parameter *p = c->parameters;
    p->Set(NVSDK_NGX_Parameter_Width, cfg->input_width);
    p->Set(NVSDK_NGX_Parameter_Height, cfg->input_height);
    p->Set(NVSDK_NGX_Parameter_OutWidth, cfg->output_width);
    p->Set(NVSDK_NGX_Parameter_OutHeight, cfg->output_height);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue,
           static_cast<int>(cfg->quality));
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (cfg->auto_exposure) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
    p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    size_t scratch = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetScratchBufferSize(
        NVSDK_NGX_Feature_SuperSampling, p, &scratch);
    if (ngx_status(c, r) != RSX_DLSS_OK) return RSX_DLSS_NGX_ERROR;
    if (scratch != 0) return RSX_DLSS_UNAVAILABLE; /* No hidden allocation. */
    r = NVSDK_NGX_VULKAN_CreateFeature1(c->device.device, command,
                                        NVSDK_NGX_Feature_SuperSampling,
                                        p, &c->feature);
    if (ngx_status(c, r) != RSX_DLSS_OK) {
        c->creation_failed = true;
        return RSX_DLSS_NGX_ERROR;
    }
    if (!c->feature) {
        c->creation_failed = true;
        return RSX_DLSS_UNAVAILABLE;
    }
    if (expected_runtime_loaded(c->runtime_path, L"nvngx_dlss.dll") == 0) {
        c->creation_failed = true;
        return RSX_DLSS_UNAVAILABLE;
    }
    c->config = *cfg;
    c->report.configured = 1;
    c->force_reset = true;
    return RSX_DLSS_OK;
}

extern "C" RSX_DLSS_API rsx_dlss_status rsx_dlss_evaluate(
    rsx_dlss_vk *c, VkCommandBuffer command, const rsx_dlss_frame *f)
{
    if (!c || !f || command == VK_NULL_HANDLE || !c->report.configured)
        return RSX_DLSS_BAD_STATE;
    c->report.last_evaluation_succeeded = 0;
    const rsx_dlss_config &cfg = c->config;
    if (f->config_generation != cfg.generation || !f->frame_serial ||
        f->motion_frame_serial != f->frame_serial ||
        f->motion_provenance != RSX_DLSS_MOTION_CAMERA_AND_OBJECT_UNJITTERED ||
        f->reset > 1 || !std::isfinite(f->jitter_x) ||
        !std::isfinite(f->jitter_y) || !std::isfinite(f->delta_milliseconds) ||
        f->delta_milliseconds <= 0 || !std::isfinite(f->pre_exposure) ||
        f->pre_exposure <= 0 || !std::isfinite(f->exposure_scale) ||
        f->exposure_scale <= 0 ||
        !image_ok(f->color, VK_FORMAT_R16G16B16A16_SFLOAT,
                  cfg.input_width, cfg.input_height,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
        !image_ok(f->depth, VK_FORMAT_R32_SFLOAT,
                  cfg.input_width, cfg.input_height,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
        !image_ok(f->motion, VK_FORMAT_R32G32_SFLOAT,
                  cfg.input_width, cfg.input_height,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
        !image_ok(f->output, VK_FORMAT_R16G16B16A16_SFLOAT,
                  cfg.output_width, cfg.output_height, VK_IMAGE_LAYOUT_GENERAL))
        return RSX_DLSS_BAD_ARGUMENT;
    if (f->output.image == f->color.image ||
        f->output.image == f->depth.image ||
        f->output.image == f->motion.image) return RSX_DLSS_BAD_ARGUMENT;
    if (c->report.evaluation_count &&
        f->frame_serial <= c->report.last_frame_serial) return RSX_DLSS_BAD_ARGUMENT;
    bool consecutive = c->report.evaluation_count &&
                       c->report.last_frame_serial != UINT64_MAX &&
                       f->frame_serial == c->report.last_frame_serial + 1;
    if ((!consecutive || c->force_reset) && !f->reset)
        return RSX_DLSS_BAD_ARGUMENT;
    NVSDK_NGX_Resource_VK color = ngx_image(f->color, false);
    NVSDK_NGX_Resource_VK depth = ngx_image(f->depth, false);
    NVSDK_NGX_Resource_VK motion = ngx_image(f->motion, false);
    NVSDK_NGX_Resource_VK output = ngx_image(f->output, true);
    NVSDK_NGX_Parameter *p = c->parameters;
    p->Set(NVSDK_NGX_Parameter_Color, static_cast<void *>(&color));
    p->Set(NVSDK_NGX_Parameter_Depth, static_cast<void *>(&depth));
    p->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<void *>(&motion));
    p->Set(NVSDK_NGX_Parameter_Output, static_cast<void *>(&output));
    p->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, f->jitter_x);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, f->jitter_y);
    p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, f->delta_milliseconds);
    p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, f->pre_exposure);
    p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, f->exposure_scale);
    p->Set(NVSDK_NGX_Parameter_Reset, static_cast<int>(f->reset));
    p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
           cfg.input_width);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
           cfg.input_height);
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_EvaluateFeature_C(
        command, c->feature, p, nullptr);
    if (ngx_status(c, r) != RSX_DLSS_OK) {
        c->force_reset = true;
        return RSX_DLSS_NGX_ERROR;
    }
    if (expected_runtime_loaded(c->runtime_path, L"nvngx_dlss.dll") != 1) {
        c->force_reset = true;
        return RSX_DLSS_UNAVAILABLE;
    }
    c->force_reset = false;
    c->report.last_evaluation_succeeded = 1;
    ++c->report.evaluation_count;
    c->report.last_frame_serial = f->frame_serial;
    return RSX_DLSS_OK;
}

namespace {
static void fg_result(rsx_dlss_fg_report &report, NVSDK_NGX_Result r)
{
    report.ngx_result = static_cast<int32_t>(r);
}
static bool fg_identity(const rsx_dlss_identity *id, Discovery &d,
                        rsx_dlss_fg_report *report)
{
    if (!d.fill(id)) return false; /* Validate identity and base SR runtime. */
    if (!named_runtime_exists(d.runtime_path, L"nvngx_dlssg.dll")) {
        if (report) report->cause = RSX_DLSS_FG_RUNTIME_MISSING;
        return false;
    }
    d.info.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    return true;
}
static NVSDK_NGX_FeatureDiscoveryInfo fg_discovery(const rsx_dlss_vk *c)
{
    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc = {c->project.c_str(),
                                     NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                     "folklore-native-vulkan"};
    info.ApplicationDataPath = c->log_path.c_str();
    info.FeatureInfo = &c->common;
    return info;
}
static bool finite_array(const float *values, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        if (!std::isfinite(values[i])) return false;
    return true;
}
static bool inverse_pair(const float *a, const float *b)
{
    if (!finite_array(a, 16) || !finite_array(b, 16)) return false;
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col) {
            double sum = 0;
            for (unsigned k = 0; k < 4; ++k)
                sum += static_cast<double>(a[row * 4 + k]) * b[k * 4 + col];
            if (!std::isfinite(sum) ||
                std::fabs(sum - (row == col ? 1.0 : 0.0)) > 0.05) return false;
        }
    return true;
}
static bool fg_camera_ok(const rsx_dlss_fg_camera &v)
{
    if (!inverse_pair(v.view_to_clip, v.clip_to_view) ||
        !inverse_pair(v.clip_to_prev_clip, v.prev_clip_to_clip) ||
        !finite_array(v.clip_to_lens_clip, 16) ||
        std::fabs(v.clip_to_lens_clip[15]) < 0.00001f ||
        !finite_array(v.position, 3) || !finite_array(v.up, 3) ||
        !finite_array(v.right, 3) || !finite_array(v.forward, 3) ||
         !std::isfinite(v.jitter_clip_x) || !std::isfinite(v.jitter_clip_y) ||
        !std::isfinite(v.near_plane) || !std::isfinite(v.far_plane) ||
        !std::isfinite(v.fov_radians) || !std::isfinite(v.aspect_ratio) ||
        v.near_plane <= 0 || v.far_plane <= v.near_plane ||
        v.fov_radians <= 0 || v.fov_radians >= 3.141593f ||
        v.aspect_ratio <= 0) return false;
    for (const float *axis : {v.up, v.right, v.forward}) {
        double length2 = static_cast<double>(axis[0]) * axis[0] +
                         static_cast<double>(axis[1]) * axis[1] +
                         static_cast<double>(axis[2]) * axis[2];
        if (length2 < 0.25 || length2 > 4.0) return false;
    }
    return true;
}
static void fg_parameters_for_frame(NVSDK_NGX_Parameter *p,
                                    const rsx_dlss_fg_frame *f,
                                    const rsx_dlss_fg_config &cfg,
                                    NVSDK_NGX_Resource_VK *resources)
{
    p->Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, static_cast<void *>(&resources[0]));
    p->Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, static_cast<void *>(&resources[1]));
    p->Set(NVSDK_NGX_DLSSG_Parameter_Depth, static_cast<void *>(&resources[2]));
    p->Set(NVSDK_NGX_DLSSG_Parameter_MVecs, static_cast<void *>(&resources[3]));
    p->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated,
           static_cast<void *>(&resources[4]));
    p->Set(NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation,
           static_cast<void *>(&resources[5]));
    p->Set(NVSDK_NGX_DLSSG_Parameter_UI, static_cast<void *>(nullptr));
    p->Set(NVSDK_NGX_DLSSG_Parameter_UIAlpha, static_cast<void *>(nullptr));
    p->Set(NVSDK_NGX_DLSSG_Parameter_OutputReal, static_cast<void *>(nullptr));
    p->Set(NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionField,
           static_cast<void *>(nullptr));
    p->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, 1u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,
           static_cast<unsigned long long>(f->frame_serial));
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraViewToClip,
           static_cast<void *>(const_cast<float *>(f->camera.view_to_clip)));
    p->Set(NVSDK_NGX_DLSSG_Parameter_ClipToCameraView,
           static_cast<void *>(const_cast<float *>(f->camera.clip_to_view)));
    p->Set(NVSDK_NGX_DLSSG_Parameter_ClipToLensClip,
           static_cast<void *>(const_cast<float *>(f->camera.clip_to_lens_clip)));
    p->Set(NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip,
           static_cast<void *>(const_cast<float *>(f->camera.clip_to_prev_clip)));
    p->Set(NVSDK_NGX_DLSSG_Parameter_PrevClipToClip,
           static_cast<void *>(const_cast<float *>(f->camera.prev_clip_to_clip)));
    p->Set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, f->camera.jitter_clip_x);
    p->Set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, f->camera.jitter_clip_y);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, 1.0f / cfg.render_width);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, 1.0f / cfg.render_height);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraPosX, f->camera.position[0]);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraPosY, f->camera.position[1]);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraPosZ, f->camera.position[2]);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraUpX, f->camera.up[0]);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraUpY, f->camera.up[1]);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraUpZ, f->camera.up[2]);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraRightX, f->camera.right[0]);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraRightY, f->camera.right[1]);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraRightZ, f->camera.right[2]);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraFwdX, f->camera.forward[0]);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraFwdY, f->camera.forward[1]);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraFwdZ, f->camera.forward[2]);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraNear, f->camera.near_plane);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraFar, f->camera.far_plane);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraFOV, f->camera.fov_radians);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, f->camera.aspect_ratio);
    p->Set(NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, cfg.color_buffers_hdr);
    p->Set(NVSDK_NGX_DLSSG_Parameter_DepthInverted, 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, 1u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_Reset, f->reset);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MvecJittered, 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectWidth, cfg.output_width);
    p->Set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectHeight, cfg.output_height);
    p->Set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectWidth, cfg.output_width);
    p->Set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectHeight, cfg.output_height);
    p->Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectWidth, cfg.render_width);
    p->Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectHeight, cfg.render_height);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, cfg.render_width);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, cfg.render_height);
    p->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectWidth,
           cfg.output_width);
    p->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectHeight,
           cfg.output_height);
}
}

extern "C" RSX_DLSS_API rsx_dlss_status rsx_dlss_fg_instance_extensions(
    const rsx_dlss_identity *identity, rsx_dlss_extensions *out,
    rsx_dlss_fg_report *report)
{
    if (!out || !report) return RSX_DLSS_BAD_ARGUMENT;
    *report = {}; out->count = 0;
    try {
        Discovery d;
        if (!fg_identity(identity, d, report)) return report->cause ==
            RSX_DLSS_FG_RUNTIME_MISSING ? RSX_DLSS_UNAVAILABLE : RSX_DLSS_BAD_ARGUMENT;
        uint32_t count = 0; VkExtensionProperties *items = nullptr;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
            &d.info, &count, &items);
        fg_result(*report, r);
        if (NVSDK_NGX_FAILED(r)) {
            report->cause = RSX_DLSS_FG_REQUIREMENTS_FAILED;
            return RSX_DLSS_NGX_ERROR;
        }
        rsx_dlss_status copied = copy_extensions(count, items, out);
        if (copied != RSX_DLSS_OK)
            report->cause = RSX_DLSS_FG_REQUIREMENTS_FAILED;
        return copied;
    } catch (const std::bad_alloc &) { return RSX_DLSS_NO_MEMORY; }
    catch (...) { return RSX_DLSS_BAD_ARGUMENT; }
}

extern "C" RSX_DLSS_API rsx_dlss_status rsx_dlss_fg_device_extensions(
    const rsx_dlss_identity *identity, VkInstance instance,
    VkPhysicalDevice physical, rsx_dlss_extensions *out,
    rsx_dlss_fg_report *report)
{
    if (!out || !report || instance == VK_NULL_HANDLE ||
        physical == VK_NULL_HANDLE) return RSX_DLSS_BAD_ARGUMENT;
    *report = {}; out->count = 0;
    try {
        Discovery d;
        if (!fg_identity(identity, d, report)) return report->cause ==
            RSX_DLSS_FG_RUNTIME_MISSING ? RSX_DLSS_UNAVAILABLE : RSX_DLSS_BAD_ARGUMENT;
        uint32_t count = 0; VkExtensionProperties *items = nullptr;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
            instance, physical, &d.info, &count, &items);
        fg_result(*report, r);
        if (NVSDK_NGX_FAILED(r)) {
            report->cause = RSX_DLSS_FG_REQUIREMENTS_FAILED;
            return RSX_DLSS_NGX_ERROR;
        }
        rsx_dlss_status copied = copy_extensions(count, items, out);
        if (copied != RSX_DLSS_OK)
            report->cause = RSX_DLSS_FG_REQUIREMENTS_FAILED;
        return copied;
    } catch (const std::bad_alloc &) { return RSX_DLSS_NO_MEMORY; }
    catch (...) { return RSX_DLSS_BAD_ARGUMENT; }
}

extern "C" RSX_DLSS_API rsx_dlss_status rsx_dlss_fg_capability(
    rsx_dlss_vk *c, rsx_dlss_fg_report *out)
{
    if (!c || !out) return RSX_DLSS_BAD_ARGUMENT;
    rsx_dlss_fg_report &v = c->fg_report;
    if (c->fg_creation_failed) { *out = v; return RSX_DLSS_UNAVAILABLE; }
    if (v.configured) {
        *out = v;
        return v.cause == RSX_DLSS_FG_READY ? RSX_DLSS_OK : RSX_DLSS_UNAVAILABLE;
    }
    v = {};
    if (!named_runtime_exists(c->runtime_path, L"nvngx_dlssg.dll")) {
        v.cause = RSX_DLSS_FG_RUNTIME_MISSING;
        *out = v; return RSX_DLSS_UNAVAILABLE;
    }
    NVSDK_NGX_FeatureDiscoveryInfo info = fg_discovery(c);
    NVSDK_NGX_FeatureRequirement required{};
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureRequirements(
        c->device.instance, c->device.physical, &info, &required);
    fg_result(v, r);
    if (NVSDK_NGX_FAILED(r)) {
        v.cause = RSX_DLSS_FG_REQUIREMENTS_FAILED;
        *out = v; return RSX_DLSS_NGX_ERROR;
    }
    v.support_flags = static_cast<uint32_t>(required.FeatureSupported);
    if (v.support_flags) {
        v.cause = RSX_DLSS_FG_UNSUPPORTED_DEVICE;
        *out = v; return RSX_DLSS_UNAVAILABLE;
    }
    if (!c->fg_parameters) {
        r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&c->fg_parameters);
        fg_result(v, r);
        if (NVSDK_NGX_FAILED(r) || !c->fg_parameters) {
            v.cause = RSX_DLSS_FG_CAPABILITY_FAILED;
            *out = v; return RSX_DLSS_NGX_ERROR;
        }
    }
    int available = 0, needs_driver = 0;
    r = c->fg_parameters->Get(NVSDK_NGX_Parameter_FrameGeneration_Available,
                               &available);
    fg_result(v, r);
    if (NVSDK_NGX_FAILED(r)) {
        v.cause = RSX_DLSS_FG_CAPABILITY_FAILED;
        *out = v; return RSX_DLSS_NGX_ERROR;
    }
    r = c->fg_parameters->Get(
        NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver,
        &needs_driver);
    fg_result(v, r);
    if (NVSDK_NGX_FAILED(r)) {
        v.cause = RSX_DLSS_FG_CAPABILITY_FAILED;
        *out = v; return RSX_DLSS_NGX_ERROR;
    }
    v.available = available != 0;
    v.needs_driver_update = needs_driver != 0;
    v.cause = needs_driver ? RSX_DLSS_FG_DRIVER_UPDATE :
              !available ? RSX_DLSS_FG_FEATURE_UNAVAILABLE : RSX_DLSS_FG_READY;
    *out = v;
    return v.cause == RSX_DLSS_FG_READY ? RSX_DLSS_OK : RSX_DLSS_UNAVAILABLE;
}

extern "C" RSX_DLSS_API rsx_dlss_status rsx_dlss_fg_get_report(
    const rsx_dlss_vk *c, rsx_dlss_fg_report *out)
{
    if (!c || !out) return RSX_DLSS_BAD_ARGUMENT;
    *out = c->fg_report;
    return RSX_DLSS_OK;
}

extern "C" RSX_DLSS_API rsx_dlss_status rsx_dlss_fg_configure(
    rsx_dlss_vk *c, VkCommandBuffer command, const rsx_dlss_fg_config *cfg)
{
    if (!c || !cfg || command == VK_NULL_HANDLE || c->fg_report.configured ||
        c->fg_creation_failed || !cfg->generation || !cfg->output_width ||
        !cfg->output_height || !cfg->render_width || !cfg->render_height ||
        cfg->render_width > cfg->output_width ||
         cfg->render_height > cfg->output_height ||
         cfg->color_buffers_hdr > 1) return RSX_DLSS_BAD_ARGUMENT;
    rsx_dlss_fg_report report{};
    rsx_dlss_status status = rsx_dlss_fg_capability(c, &report);
    if (status != RSX_DLSS_OK) return status;
    NVSDK_NGX_Parameter *p = c->fg_parameters;
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_Width, cfg->output_width);
    p->Set(NVSDK_NGX_Parameter_Height, cfg->output_height);
    p->Set(NVSDK_NGX_DLSSG_Parameter_Width, cfg->output_width);
    p->Set(NVSDK_NGX_DLSSG_Parameter_Height, cfg->output_height);
    p->Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, cfg->render_width);
    p->Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, cfg->render_height);
    p->Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat,
           static_cast<unsigned int>(VK_FORMAT_R16G16B16A16_SFLOAT));
    p->Set(NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags,
           static_cast<unsigned int>(
               NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer |
               NVSDK_NGX_DLSSG_ResourceFlags_MVecs |
               NVSDK_NGX_DLSSG_ResourceFlags_Depth |
               NVSDK_NGX_DLSSG_ResourceFlags_HUDLess |
                NVSDK_NGX_DLSSG_ResourceFlags_OutputInterpolated |
                NVSDK_NGX_DLSSG_ResourceFlags_OutputDisableInterpolation));
    p->Set(NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled, 0u);
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_CreateFeature1(
        c->device.device, command, NVSDK_NGX_Feature_FrameGeneration,
        p, &c->fg_feature);
    fg_result(c->fg_report, r);
    if (NVSDK_NGX_FAILED(r) || !c->fg_feature) {
        c->fg_creation_failed = true;
        c->fg_report.cause = RSX_DLSS_FG_FEATURE_UNAVAILABLE;
        return NVSDK_NGX_FAILED(r) ? RSX_DLSS_NGX_ERROR : RSX_DLSS_UNAVAILABLE;
    }
    if (expected_runtime_loaded(c->runtime_path, L"nvngx_dlssg.dll") == 0) {
        c->fg_creation_failed = true;
        c->fg_report.cause = RSX_DLSS_FG_RUNTIME_PATH_MISMATCH;
        return RSX_DLSS_UNAVAILABLE;
    }
    c->fg_config = *cfg;
    c->fg_report.configured = 1;
    c->fg_force_reset = true;
    return RSX_DLSS_OK;
}

extern "C" RSX_DLSS_API rsx_dlss_status rsx_dlss_fg_evaluate(
    rsx_dlss_vk *c, VkCommandBuffer command, const rsx_dlss_fg_frame *f)
{
    if (!c || !f || command == VK_NULL_HANDLE || !c->fg_report.configured)
        return RSX_DLSS_BAD_STATE;
    c->fg_report.last_record_succeeded = 0;
    const rsx_dlss_fg_config &cfg = c->fg_config;
    if (f->config_generation != cfg.generation || !f->frame_serial ||
        f->motion_frame_serial != f->frame_serial ||
        f->motion_provenance != RSX_DLSS_MOTION_CAMERA_AND_OBJECT_UNJITTERED ||
        f->reset > 1 || !fg_camera_ok(f->camera) ||
        !image_ok(f->backbuffer, VK_FORMAT_R16G16B16A16_SFLOAT,
                  cfg.output_width, cfg.output_height,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
        !image_ok(f->hudless, VK_FORMAT_R16G16B16A16_SFLOAT,
                  cfg.output_width, cfg.output_height,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
        !image_ok(f->depth, VK_FORMAT_R32_SFLOAT,
                  cfg.render_width, cfg.render_height,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
        !image_ok(f->motion, VK_FORMAT_R32G32_SFLOAT,
                  cfg.render_width, cfg.render_height,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
         !image_ok(f->output_interpolated, VK_FORMAT_R16G16B16A16_SFLOAT,
                   cfg.output_width, cfg.output_height, VK_IMAGE_LAYOUT_GENERAL) ||
         f->output_disable_interpolation.buffer == VK_NULL_HANDLE ||
         f->output_disable_interpolation.size_bytes < 4 ||
         (f->output_disable_interpolation.declared_usage &
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0)
        return RSX_DLSS_BAD_ARGUMENT;
    if (f->output_interpolated.image == f->backbuffer.image ||
        f->output_interpolated.image == f->hudless.image ||
        f->output_interpolated.image == f->depth.image ||
        f->output_interpolated.image == f->motion.image)
        return RSX_DLSS_BAD_ARGUMENT;
    if (c->fg_report.record_count &&
        f->frame_serial <= c->fg_report.last_frame_serial)
        return RSX_DLSS_BAD_ARGUMENT;
    bool consecutive = c->fg_report.record_count &&
        c->fg_report.last_frame_serial != UINT64_MAX &&
        f->frame_serial == c->fg_report.last_frame_serial + 1;
    if ((!consecutive || c->fg_force_reset) && !f->reset)
        return RSX_DLSS_BAD_ARGUMENT;
    NVSDK_NGX_Resource_VK resources[6] = {
        ngx_image(f->backbuffer, false), ngx_image(f->hudless, false),
        ngx_image(f->depth, false), ngx_image(f->motion, false),
        ngx_image(f->output_interpolated, true), {}
    };
    resources[5].Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER;
    resources[5].ReadWrite = true;
    resources[5].Resource.BufferInfo.Buffer =
        f->output_disable_interpolation.buffer;
    resources[5].Resource.BufferInfo.SizeInBytes =
        f->output_disable_interpolation.size_bytes;
    fg_parameters_for_frame(c->fg_parameters, f, cfg, resources);
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_EvaluateFeature_C(
        command, c->fg_feature, c->fg_parameters, nullptr);
    fg_result(c->fg_report, r);
    if (NVSDK_NGX_FAILED(r)) {
        c->fg_force_reset = true;
        return RSX_DLSS_NGX_ERROR;
    }
    if (expected_runtime_loaded(c->runtime_path, L"nvngx_dlssg.dll") != 1) {
        c->fg_report.cause = RSX_DLSS_FG_RUNTIME_PATH_MISMATCH;
        c->fg_force_reset = true;
        return RSX_DLSS_UNAVAILABLE;
    }
    c->fg_report.last_record_succeeded = 1;
    ++c->fg_report.record_count;
    c->fg_report.last_frame_serial = f->frame_serial;
    c->fg_force_reset = false;
    return RSX_DLSS_OK;
}

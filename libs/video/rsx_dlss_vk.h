#ifndef RSX_DLSS_VK_H
#define RSX_DLSS_VK_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#if defined(_WIN32)
# if defined(RSX_DLSS_VK_BUILD)
#  define RSX_DLSS_API __declspec(dllexport)
# else
#  define RSX_DLSS_API __declspec(dllimport)
# endif
#else
# define RSX_DLSS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_dlss_vk rsx_dlss_vk;

typedef enum rsx_dlss_status {
    RSX_DLSS_OK = 0,
    RSX_DLSS_BAD_ARGUMENT = 1,
    RSX_DLSS_BAD_STATE = 2,
    RSX_DLSS_UNAVAILABLE = 3,
    RSX_DLSS_NGX_ERROR = 4,
    RSX_DLSS_NO_MEMORY = 5
} rsx_dlss_status;

typedef enum rsx_dlss_quality {
    RSX_DLSS_PERFORMANCE = 0,
    RSX_DLSS_BALANCED = 1,
    RSX_DLSS_QUALITY = 2,
    RSX_DLSS_ULTRA_PERFORMANCE = 3,
    RSX_DLSS_ULTRA_QUALITY = 4,
    RSX_DLSS_DLAA = 5
} rsx_dlss_quality;

/* UTF-16 paths must be absolute existing directories. Runtime directory must
 * contain the official nvngx_dlss.dll. UUID is a caller-owned 36-byte UUID
 * string, excluding NUL. Discovery and context must use identical identity. */
typedef struct rsx_dlss_identity {
    const char *project_uuid;
    const uint16_t *log_directory_utf16;
    const uint16_t *runtime_directory_utf16;
} rsx_dlss_identity;

#define RSX_DLSS_MAX_EXTENSIONS 32u
typedef struct rsx_dlss_extensions {
    uint32_t count;
    VkExtensionProperties items[RSX_DLSS_MAX_EXTENSIONS];
} rsx_dlss_extensions;

/* Call before vkCreateInstance/vkCreateDevice, merge returned requirements
 * into the caller's enabled-extension lists, and verify local Vulkan support.
 * No pointer returned by NGX is retained. ngx_result receives the raw code. */
RSX_DLSS_API rsx_dlss_status rsx_dlss_instance_extensions(
    const rsx_dlss_identity *identity, rsx_dlss_extensions *out,
    int32_t *ngx_result);
RSX_DLSS_API rsx_dlss_status rsx_dlss_device_extensions(
    const rsx_dlss_identity *identity, VkInstance instance,
    VkPhysicalDevice physical, rsx_dlss_extensions *out,
    int32_t *ngx_result);

typedef struct rsx_dlss_device {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    PFN_vkGetInstanceProcAddr get_instance_proc;
    PFN_vkGetDeviceProcAddr get_device_proc;
} rsx_dlss_device;

typedef struct rsx_dlss_report {
    int32_t ngx_result;              /* Most recent NGX call, separate from status. */
    uint32_t support_flags;          /* 0 means feature requirements supported. */
    uint32_t available;
    uint32_t needs_driver_update;
    uint32_t configured;             /* Feature creation recorded successfully. */
    uint32_t last_evaluation_succeeded; /* NGX command recording only. */
    uint64_t evaluation_count;         /* Successful NGX records, not GPU completions. */
    uint64_t last_frame_serial;
} rsx_dlss_report;

/* One owning NGX context per borrowed VkDevice in the process. Caller must
 * keep the device alive; drain GPU before
 * destroying this context or reconfiguring a feature. One owning render thread.
 * No device creation, queue submission, wait, OTA or implicit runtime search. */
RSX_DLSS_API rsx_dlss_status rsx_dlss_create(
    const rsx_dlss_identity *identity, const rsx_dlss_device *device,
    rsx_dlss_vk **out, rsx_dlss_report *report);
RSX_DLSS_API void rsx_dlss_destroy(rsx_dlss_vk *context);
RSX_DLSS_API rsx_dlss_status rsx_dlss_get_report(
    const rsx_dlss_vk *context, rsx_dlss_report *out);

typedef struct rsx_dlss_size {
    uint32_t input_width, input_height;
    uint32_t minimum_width, minimum_height;
    uint32_t maximum_width, maximum_height;
} rsx_dlss_size;

RSX_DLSS_API rsx_dlss_status rsx_dlss_optimal_size(
    rsx_dlss_vk *context, uint32_t output_width, uint32_t output_height,
    rsx_dlss_quality quality, rsx_dlss_size *out);

typedef struct rsx_dlss_config {
    uint64_t generation;             /* Nonzero; change on every reconfiguration. */
    uint32_t input_width, input_height;
    uint32_t output_width, output_height;
    rsx_dlss_quality quality;
    uint32_t auto_exposure;          /* 0 or 1. Input is linear HDR RGBA16F. */
} rsx_dlss_config;

/* Records NGX feature creation on a caller-owned recording command buffer.
 * The caller submits and drains that command before first evaluation. If
 * creation fails, discard that command buffer and destroy this context. The
 * loaded nvngx_dlss.dll path is checked against the explicit runtime path.
 * No reconfiguration here: destroy after GPU drain, then create a new context. */
RSX_DLSS_API rsx_dlss_status rsx_dlss_configure(
    rsx_dlss_vk *context, VkCommandBuffer command,
    const rsx_dlss_config *config);

typedef struct rsx_dlss_image {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
    VkImageSubresourceRange subresource;
    VkImageLayout declared_layout;
} rsx_dlss_image;

/* Motion provenance is an explicit renderer assertion, not verified pixels.
 * Producer must include camera AND object motion from actual current/previous
 * post-skinning geometry, in previous-minus-current UNJITTERED input pixels.
 * Supply RG32F low-resolution vectors and matching frame serial. A static
 * zero-vector field is valid only when the complete scene is really static. */
#define RSX_DLSS_MOTION_CAMERA_AND_OBJECT_UNJITTERED 0x4D56434Fu
typedef struct rsx_dlss_frame {
    uint64_t config_generation;
    uint64_t frame_serial;
    uint64_t motion_frame_serial;
    uint32_t motion_provenance;
    rsx_dlss_image color;             /* RGBA16F, input extent, shader-read layout. */
    rsx_dlss_image depth;             /* R32F, input extent, shader-read layout. */
    rsx_dlss_image motion;            /* RG32F, input extent, shader-read layout. */
    rsx_dlss_image output;            /* RGBA16F, output extent, GENERAL layout. */
    float jitter_x, jitter_y;        /* Input pixels, +x right, +y down. */
    float delta_milliseconds;        /* Finite and > 0. */
    float pre_exposure;              /* Finite and > 0. */
    float exposure_scale;            /* Finite and > 0. */
    uint32_t reset;                  /* 1 on first frame or history discontinuity. */
} rsx_dlss_frame;

/* Caller owns image memory, usage bits, barriers and real Vulkan layouts.
 * Image views must select color aspect, mip 0, one level, layer 0, one layer;
 * images must be 2D, single-sample, and support sampled input/storage output.
 * The declared layouts are checked, but only the caller can ensure they match
 * actual command-buffer state. The function only records NGX evaluation. If
 * NGX returns an error, discard that command buffer; the next attempt must
 * request reset. A successful record is not proof of GPU execution. */
RSX_DLSS_API rsx_dlss_status rsx_dlss_evaluate(
    rsx_dlss_vk *context, VkCommandBuffer command,
    const rsx_dlss_frame *frame);

/* Optional DLSS Frame Generation (2x). This path uses the SAME NGX device
 * context but distinct capability parameters and feature state. Missing
 * nvngx_dlssg.dll leaves Super Resolution available. Required FG extensions
 * must be queried before creating Vulkan instance/device. No generated frame
 * is submitted, timed, presented, or declared visually correct here. */
typedef enum rsx_dlss_fg_cause {
    RSX_DLSS_FG_NOT_QUERIED = 0,
    RSX_DLSS_FG_READY,
    RSX_DLSS_FG_RUNTIME_MISSING,
    RSX_DLSS_FG_REQUIREMENTS_FAILED,
    RSX_DLSS_FG_UNSUPPORTED_DEVICE,
    RSX_DLSS_FG_CAPABILITY_FAILED,
    RSX_DLSS_FG_DRIVER_UPDATE,
    RSX_DLSS_FG_FEATURE_UNAVAILABLE,
    RSX_DLSS_FG_RUNTIME_PATH_MISMATCH
} rsx_dlss_fg_cause;

typedef struct rsx_dlss_fg_report {
    rsx_dlss_fg_cause cause;
    int32_t ngx_result;
    uint32_t support_flags;
    uint32_t available;
    uint32_t needs_driver_update;
    uint32_t configured;
    uint32_t last_record_succeeded;
    uint64_t record_count;
    uint64_t last_frame_serial;
} rsx_dlss_fg_report;

RSX_DLSS_API rsx_dlss_status rsx_dlss_fg_instance_extensions(
    const rsx_dlss_identity *identity, rsx_dlss_extensions *out,
    rsx_dlss_fg_report *report);
RSX_DLSS_API rsx_dlss_status rsx_dlss_fg_device_extensions(
    const rsx_dlss_identity *identity, VkInstance instance,
    VkPhysicalDevice physical, rsx_dlss_extensions *out,
    rsx_dlss_fg_report *report);
RSX_DLSS_API rsx_dlss_status rsx_dlss_fg_capability(
    rsx_dlss_vk *context, rsx_dlss_fg_report *report);
RSX_DLSS_API rsx_dlss_status rsx_dlss_fg_get_report(
    const rsx_dlss_vk *context, rsx_dlss_fg_report *report);

typedef struct rsx_dlss_fg_config {
    uint64_t generation;
    uint32_t output_width, output_height;
    uint32_t render_width, render_height;
    uint32_t color_buffers_hdr; /* 0 = SDR, 1 = full HDR color pipeline. */
} rsx_dlss_fg_config;

/* Records FG feature creation. Caller submits and drains command buffer
 * before first evaluation; creation failure requires context destruction. */
RSX_DLSS_API rsx_dlss_status rsx_dlss_fg_configure(
    rsx_dlss_vk *context, VkCommandBuffer command,
    const rsx_dlss_fg_config *config);

typedef struct rsx_dlss_fg_camera {
    /* Row-major 4x4, all UNJITTERED; inverse pairs must agree. */
    float view_to_clip[16], clip_to_view[16];
    float clip_to_lens_clip[16];
    float clip_to_prev_clip[16], prev_clip_to_clip[16];
    float position[3], up[3], right[3], forward[3];
    float near_plane, far_plane, fov_radians, aspect_ratio;
    float jitter_clip_x, jitter_clip_y; /* Current NGX clip-space jitter. */
} rsx_dlss_fg_camera;

typedef struct rsx_dlss_fg_output_flag {
    VkBuffer buffer;
    uint32_t size_bytes; /* At least 4. NGX writes suppression to byte 0. */
    VkBufferUsageFlags declared_usage; /* Includes STORAGE_BUFFER. */
} rsx_dlss_fg_output_flag;

typedef struct rsx_dlss_fg_frame {
    uint64_t config_generation;
    uint64_t frame_serial; /* Real rendered backbuffer ID. */
    uint64_t motion_frame_serial;
    uint32_t motion_provenance; /* Same required producer assertion as SR. */
    rsx_dlss_image backbuffer;          /* RGBA16F rendered frame, output extent. */
    rsx_dlss_image hudless;             /* RGBA16F output extent. */
    rsx_dlss_image depth;               /* R32F render extent. */
    rsx_dlss_image motion;              /* RG32F render extent. */
    rsx_dlss_image output_interpolated; /* RGBA16F output extent, GENERAL. */
    rsx_dlss_fg_output_flag output_disable_interpolation;
    rsx_dlss_fg_camera camera;
    uint32_t reset; /* Required on first frame and after serial gap. */
} rsx_dlss_fg_frame;

/* All input views shader-read; output GENERAL. Caller clears the suppression
 * buffer to zero before NGX, barriers transfer->compute and compute->host, then
 * reads byte 0 only after GPU completion. A nonzero byte forbids presentation.
 * Caller owns resource usage, barriers, submission, completion and presentation.
 * SR/FG do not prove game motion, interpolation quality or delivered frames. */
RSX_DLSS_API rsx_dlss_status rsx_dlss_fg_evaluate(
    rsx_dlss_vk *context, VkCommandBuffer command,
    const rsx_dlss_fg_frame *frame);

#ifdef __cplusplus
}
#endif
#endif

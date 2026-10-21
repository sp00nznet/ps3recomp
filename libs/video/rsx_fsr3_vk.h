#ifndef RSX_FSR3_VK_H
#define RSX_FSR3_VK_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#if defined(_WIN32)
# if defined(RSX_FSR3_VK_BUILD)
#  define RSX_FSR3_API __declspec(dllexport)
# else
#  define RSX_FSR3_API __declspec(dllimport)
# endif
#else
# define RSX_FSR3_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Optional C ABI bridge to the AMD FSR 3.1.4 upscaler (FidelityFX SDK 1.1.4,
 * MIT), shaped like rsx_dlss_vk. Upscaling only: no frame generation, no
 * swapchain replacement, no queue submission or wait. One owning render
 * thread per context. */
typedef struct rsx_fsr3_vk rsx_fsr3_vk;

typedef enum rsx_fsr3_status {
    RSX_FSR3_OK = 0,
    RSX_FSR3_BAD_ARGUMENT = 1,
    RSX_FSR3_BAD_STATE = 2,
    RSX_FSR3_UNAVAILABLE = 3,
    RSX_FSR3_FFX_ERROR = 4,
    RSX_FSR3_NO_MEMORY = 5
} rsx_fsr3_status;

/* Per-dimension ratios 1.0, 1.5, 1.7, 2.0, 3.0 (FfxFsr3UpscalerQualityMode). */
typedef enum rsx_fsr3_quality {
    RSX_FSR3_NATIVE_AA = 0,
    RSX_FSR3_QUALITY = 1,
    RSX_FSR3_BALANCED = 2,
    RSX_FSR3_PERFORMANCE = 3,
    RSX_FSR3_ULTRA_PERFORMANCE = 4
} rsx_fsr3_quality;

/* What the FSR 3.1 SPIR-V needs, taken from the shaders' capabilities.
 * required: OR into VkPhysicalDeviceFeatures before vkCreateDevice.
 * fp16_supported: shaderFloat16 (Vulkan 1.2) and shaderInt16 are available;
 * the 16-bit shaders are used only if the caller also enables both and says
 * so in rsx_fsr3_device.fp16_enabled. No device extension is required. */
typedef struct rsx_fsr3_requirements {
    VkPhysicalDeviceFeatures required;
    uint32_t fp16_supported;
} rsx_fsr3_requirements;

/* UNAVAILABLE when the device lacks Vulkan 1.2, a required feature, or
 * basic+quad subgroup operations in compute shaders. */
RSX_FSR3_API rsx_fsr3_status rsx_fsr3_device_requirements(
    VkPhysicalDevice physical, rsx_fsr3_requirements *out);

typedef struct rsx_fsr3_device {
    VkPhysicalDevice physical;
    VkDevice device;
    PFN_vkGetDeviceProcAddr get_device_proc;
    uint32_t fp16_enabled; /* 1 only if shaderFloat16 AND shaderInt16 were enabled. */
} rsx_fsr3_device;

typedef struct rsx_fsr3_report {
    int32_t ffx_result;              /* Most recent FidelityFX error code. */
    uint32_t configured;
    uint32_t fp16_shaders;           /* 16-bit permutations allowed. */
    uint32_t last_evaluation_succeeded; /* Command recording only. */
    uint64_t evaluation_count;       /* Successful records, not GPU completions. */
    uint64_t last_frame_serial;
    uint64_t gpu_memory_bytes;       /* FSR-owned VRAM after configure. */
} rsx_fsr3_report;

/* Caller keeps the device alive and drains the GPU before destroy. */
RSX_FSR3_API rsx_fsr3_status rsx_fsr3_create(
    const rsx_fsr3_device *device, rsx_fsr3_vk **out, rsx_fsr3_report *report);
RSX_FSR3_API void rsx_fsr3_destroy(rsx_fsr3_vk *context);
RSX_FSR3_API rsx_fsr3_status rsx_fsr3_get_report(
    const rsx_fsr3_vk *context, rsx_fsr3_report *out);

/* Render extent for an output extent and quality, and the recommended
 * jitter sequence (pixels, +x right, +y down, never both zero). */
RSX_FSR3_API rsx_fsr3_status rsx_fsr3_render_size(
    uint32_t output_width, uint32_t output_height, rsx_fsr3_quality quality,
    uint32_t *render_width, uint32_t *render_height);
RSX_FSR3_API rsx_fsr3_status rsx_fsr3_jitter(
    uint32_t render_width, uint32_t output_width, uint64_t frame_index,
    float *jitter_x, float *jitter_y, uint32_t *phase_count);

typedef enum rsx_fsr3_color {
    RSX_FSR3_COLOR_LDR = 0,          /* [0,1] perceptual, e.g. sRGB-encoded. */
    RSX_FSR3_COLOR_LINEAR_HDR = 1
} rsx_fsr3_color;

typedef struct rsx_fsr3_config {
    uint64_t generation;             /* Nonzero; change on every reconfiguration. */
    uint32_t max_render_width, max_render_height;
    uint32_t output_width, output_height;
    uint32_t color;                  /* rsx_fsr3_color. */
    uint32_t depth_inverted;         /* 1: near = 1, far = 0. */
    uint32_t depth_infinite;         /* 1: infinite far plane. */
    uint32_t auto_exposure;          /* 0 or 1. */
} rsx_fsr3_config;

/* Creates the FSR context and its shared images (no command recording; the
 * backend uploads its lookup tables inside the first evaluation). Changing a
 * configuration requires a GPU drain, destroy and a new context. */
RSX_FSR3_API rsx_fsr3_status rsx_fsr3_configure(
    rsx_fsr3_vk *context, const rsx_fsr3_config *config);

typedef struct rsx_fsr3_image {
    VkImage image;
    VkFormat format;
    uint32_t width, height;          /* The image's full extent. */
    VkImageUsageFlags declared_usage; /* As created: views are made from it. */
    VkImageLayout declared_layout;
} rsx_fsr3_image;

/* Same motion contract as rsx_dlss_vk: camera AND object motion from the
 * real current/previous geometry, previous-minus-current, UNJITTERED, in
 * render pixels, RG32F or RG16F at the render extent. */
#define RSX_FSR3_MOTION_CAMERA_AND_OBJECT_UNJITTERED 0x4D56434Fu

typedef struct rsx_fsr3_frame {
    uint64_t config_generation;
    uint64_t frame_serial;
    uint64_t motion_frame_serial;
    uint32_t motion_provenance;
    uint32_t render_width, render_height; /* <= max render extent. */
    rsx_fsr3_image color;            /* RGBA16F/RGBA8, SHADER_READ_ONLY_OPTIMAL. */
    rsx_fsr3_image depth;            /* R32F, SHADER_READ_ONLY_OPTIMAL. */
    rsx_fsr3_image motion;           /* RG32F/RG16F, SHADER_READ_ONLY_OPTIMAL. */
    rsx_fsr3_image output;           /* Output extent, storage usage, GENERAL. */
    rsx_fsr3_image reactive;         /* Optional R8_UNORM; image == 0 if absent. */
    float jitter_x, jitter_y;        /* Render pixels, +x right, +y down. */
    float delta_milliseconds;        /* Finite and > 0. */
    float pre_exposure;              /* Finite and > 0 (1 if unused). */
    float camera_near, camera_far;   /* Positive view distances; far may be large. */
    float vertical_fov_radians;      /* (0, pi). */
    float sharpness;                 /* 0 disables the RCAS pass; else (0, 1]. */
    uint32_t reset;                  /* 1 on first frame or history discontinuity. */
} rsx_fsr3_frame;

/* Records FSR into a caller-owned recording command buffer. The images are
 * returned in their declared layouts. If an error is returned, discard the
 * command buffer; the next frame must request reset. A successful record is
 * not proof of GPU execution or image quality.
 * The backend rings its views and constants over 4 evaluations: recording
 * evaluation N frees what evaluation N-3 used, so N-3 must have completed on
 * the GPU (at most 3 in flight), and an image passed here must outlive the
 * next 3 evaluations or the context. */
RSX_FSR3_API rsx_fsr3_status rsx_fsr3_evaluate(
    rsx_fsr3_vk *context, VkCommandBuffer command, const rsx_fsr3_frame *frame);

#ifdef __cplusplus
}
#endif
#endif

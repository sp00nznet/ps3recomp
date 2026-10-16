#ifndef RSX_VULKAN_ANISOTROPY_H
#define RSX_VULKAN_ANISOTROPY_H

/*
 * Anisotropic sampler selection for the native Vulkan RSX renderer.
 *
 * Reference: VkSamplerCreateInfo, Vulkan specification
 * https://registry.khronos.org/vulkan/specs/latest/man/html/VkSamplerCreateInfo.html
 * anisotropyEnable may only be VK_TRUE when the samplerAnisotropy device
 * feature was enabled at device creation; maxAnisotropy must lie in
 * [1, VkPhysicalDeviceLimits::maxSamplerAnisotropy].
 *
 * Pure CPU helpers: no Vulkan calls, no global state, no allocations.
 */

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse a user anisotropy override.
 * NULL text, "auto" and "0" yield 0 (automatic, honor guest).
 * Exactly "1", "2", "4", "8", "16" yield that level (1 = force off).
 * Anything else (empty, signs, whitespace, leading zeroes, suffixes,
 * overflow) fails. Returns 1 on success, 0 on failure; *out is written only
 * on success. NULL out always fails.
 */
int rsx_vk_anisotropy_parse_override(const char* text, uint32_t* out);

/*
 * Select anisotropy for one sampler. Writes only sampler->anisotropyEnable
 * and sampler->maxAnisotropy; NULL sampler is a no-op.
 *
 * guest_code:      RSX CONTROL0 bits 4..6 already extracted
 *                  ((control0 >> 4) & 7); 0..7 map to
 *                  1,2,4,6,8,10,12,16x. Out-of-range codes disable (1x).
 * override_level:  0 honors guest; 1 forces off; 2/4/8/16 replace the guest
 *                  level only when minFilter and magFilter are both
 *                  VK_FILTER_LINEAR (keeps nearest-filtered UI crisp).
 *                  Any other value disables (1x).
 * feature_enabled: nonzero only if samplerAnisotropy was enabled on the
 *                  VkDevice; otherwise anisotropy is never enabled.
 * device_max:      VkPhysicalDeviceLimits::maxSamplerAnisotropy. Nonfinite
 *                  or < 1 disables. Result is clamped to min(device_max, 16).
 * allow_override:  0 ignores valid overrides (including 1), e.g. vertex
 *                  texture samplers. Invalid codes/levels always disable.
 */
void rsx_vk_anisotropy_apply(VkSamplerCreateInfo* sampler, uint32_t guest_code, uint32_t override_level,
                             int feature_enabled, float device_max, int allow_override);

#ifdef __cplusplus
}
#endif

#endif

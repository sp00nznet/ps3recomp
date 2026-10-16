#include "rsx_vulkan_anisotropy.h"

#include <math.h>
#include <string.h>

/* RSX texture CONTROL0 max-aniso encoding (RPCS3 RSXTexture.cpp / gcm_enums.h). */
static const float k_guest_levels[8] = {1.0f, 2.0f, 4.0f, 6.0f, 8.0f, 10.0f, 12.0f, 16.0f};

int rsx_vk_anisotropy_parse_override(const char* text, uint32_t* out)
{
    static const struct {
        const char* text;
        uint32_t level;
    } k_values[] = {
        {"auto", 0}, {"0", 0}, {"1", 1}, {"2", 2}, {"4", 4}, {"8", 8}, {"16", 16},
    };
    size_t i;

    if (!out)
        return 0;
    if (!text) {
        *out = 0;
        return 1;
    }
    for (i = 0; i < sizeof(k_values) / sizeof(k_values[0]); ++i) {
        if (strcmp(text, k_values[i].text) == 0) {
            *out = k_values[i].level;
            return 1;
        }
    }
    return 0;
}

void rsx_vk_anisotropy_apply(VkSamplerCreateInfo* sampler, uint32_t guest_code, uint32_t override_level,
                             int feature_enabled, float device_max, int allow_override)
{
    float desired = guest_code < 8 ? k_guest_levels[guest_code] : 1.0f;
    float cap;

    if (!sampler)
        return;

    switch (override_level) {
    case 0:
        break;
    case 1:
        if (allow_override)
            desired = 1.0f;
        break;
    case 2:
    case 4:
    case 8:
    case 16:
        if (allow_override && sampler->minFilter == VK_FILTER_LINEAR && sampler->magFilter == VK_FILTER_LINEAR)
            desired = (float)override_level;
        break;
    default:
        desired = 1.0f;
        break;
    }

    if (guest_code >= 8 || !feature_enabled || !isfinite(device_max) || device_max < 1.0f || desired <= 1.0f) {
        sampler->anisotropyEnable = VK_FALSE;
        sampler->maxAnisotropy = 1.0f;
        return;
    }

    cap = device_max < 16.0f ? device_max : 16.0f;
    if (desired > cap)
        desired = cap;
    if (isfinite(desired) && desired > 1.0f) {
        sampler->anisotropyEnable = VK_TRUE;
        sampler->maxAnisotropy = desired;
    } else {
        sampler->anisotropyEnable = VK_FALSE;
        sampler->maxAnisotropy = 1.0f;
    }
}

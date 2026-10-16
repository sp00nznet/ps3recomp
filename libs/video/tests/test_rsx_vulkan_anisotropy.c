#include "../rsx_vulkan_anisotropy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_pass;
static int g_fail;

static void check(int cond, const char* what, int line, int ctx)
{
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        fprintf(stderr, "FAIL line %d (ctx %d): %s\n", line, ctx, what);
    }
}

#define CHECK(c, ctx) check(!!(c), #c, __LINE__, (int)(ctx))

static int g_pnext_dummy;

static VkSamplerCreateInfo make_sampler(VkFilter min_filter, VkFilter mag_filter)
{
    VkSamplerCreateInfo s;
    memset(&s, 0, sizeof(s));
    s.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    s.pNext = &g_pnext_dummy;
    s.flags = 0;
    s.magFilter = mag_filter;
    s.minFilter = min_filter;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    s.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    s.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.mipLodBias = 0.25f;
    s.anisotropyEnable = VK_TRUE; /* stale values the helper must overwrite */
    s.maxAnisotropy = 99.0f;
    s.compareEnable = VK_TRUE;
    s.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    s.minLod = 0.5f;
    s.maxLod = 7.0f;
    s.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    s.unnormalizedCoordinates = VK_FALSE;
    return s;
}

/* Applies the helper, verifies unrelated fields are untouched, checks the result. */
static void expect(VkFilter min_f, VkFilter mag_f, uint32_t code, uint32_t ovr, int feat, float dmax, int allow,
                   VkBool32 want_enable, float want_max, int line)
{
    VkSamplerCreateInfo s = make_sampler(min_f, mag_f);
    const VkSamplerCreateInfo b = s;
    rsx_vk_anisotropy_apply(&s, code, ovr, feat, dmax, allow);
    check(s.anisotropyEnable == want_enable && s.maxAnisotropy == want_max, "anisotropy result", line, (int)code);
    if (s.anisotropyEnable != want_enable || s.maxAnisotropy != want_max)
        fprintf(stderr, "  code=%u ovr=%u feat=%d max=%g allow=%d -> en=%u max=%g, want en=%u max=%g\n", code, ovr,
                feat, (double)dmax, allow, s.anisotropyEnable, (double)s.maxAnisotropy, want_enable,
                (double)want_max);
    check(s.sType == b.sType && s.pNext == b.pNext && s.flags == b.flags && s.magFilter == b.magFilter &&
              s.minFilter == b.minFilter && s.mipmapMode == b.mipmapMode && s.addressModeU == b.addressModeU &&
              s.addressModeV == b.addressModeV && s.addressModeW == b.addressModeW &&
              s.mipLodBias == b.mipLodBias && s.compareEnable == b.compareEnable && s.compareOp == b.compareOp &&
              s.minLod == b.minLod && s.maxLod == b.maxLod && s.borderColor == b.borderColor &&
              s.unnormalizedCoordinates == b.unnormalizedCoordinates,
          "unrelated fields preserved", line, (int)code);
}

#define LIN VK_FILTER_LINEAR
#define NEA VK_FILTER_NEAREST
#define OFF VK_FALSE, 1.0f

int main(void)
{
    static const float levels[8] = {1, 2, 4, 6, 8, 10, 12, 16};
    static const uint32_t ovrs[4] = {2, 4, 8, 16};
    static const uint32_t bad_codes[3] = {8, 100, 0xFFFFFFFFu};
    static const uint32_t bad_levels[5] = {3, 5, 15, 32, 0xFFFFFFFFu};
    static const char* good_text[7] = {"auto", "0", "1", "2", "4", "8", "16"};
    static const uint32_t good_val[7] = {0, 0, 1, 2, 4, 8, 16};
    static const char* bad_text[] = {"", " ", "+2", "-2", " 2", "2 ", "02", "00", "016", "3", "32", "2x",
                                     "AUTO", "Auto", "auto ", "16\n", "4294967312", "99999999999999999999"};
    const float bad_caps[5] = {NAN, INFINITY, -INFINITY, -4.0f, 0.5f};
    uint32_t i, j, v;

    /* All guest codes, override 0, with and without allow_override. */
    for (i = 0; i < 8; ++i) {
        VkBool32 en = i > 0 ? VK_TRUE : VK_FALSE;
        expect(LIN, LIN, i, 0, 1, 16.0f, 1, en, levels[i], __LINE__);
        expect(NEA, NEA, i, 0, 1, 16.0f, 0, en, levels[i], __LINE__);
        /* Override 1 forces off only when allowed. */
        expect(LIN, LIN, i, 1, 1, 16.0f, 1, OFF, __LINE__);
        expect(LIN, LIN, i, 1, 1, 16.0f, 0, en, levels[i], __LINE__);
        for (j = 0; j < 4; ++j) {
            expect(LIN, LIN, i, ovrs[j], 1, 16.0f, 1, VK_TRUE, (float)ovrs[j], __LINE__);
            /* Nearest min or mag, or vertex samplers: guest level wins. */
            expect(NEA, LIN, i, ovrs[j], 1, 16.0f, 1, en, levels[i], __LINE__);
            expect(LIN, NEA, i, ovrs[j], 1, 16.0f, 1, en, levels[i], __LINE__);
            expect(LIN, LIN, i, ovrs[j], 1, 16.0f, 0, en, levels[i], __LINE__);
        }
        /* Invalid override levels and unsupported feature disable. */
        for (j = 0; j < 5; ++j) {
            expect(LIN, LIN, i, bad_levels[j], 1, 16.0f, 1, OFF, __LINE__);
            expect(LIN, LIN, i, bad_levels[j], 1, 16.0f, 0, OFF, __LINE__);
            expect(LIN, LIN, i, 0, 1, bad_caps[j], 1, OFF, __LINE__);
        }
        expect(LIN, LIN, i, 0, 0, 16.0f, 1, OFF, __LINE__);
        expect(LIN, LIN, i, 16, 0, 16.0f, 1, OFF, __LINE__);
        expect(LIN, LIN, i, 16, 1, 1.0f, 1, OFF, __LINE__);
        /* Caps: fractional honored exactly, >16 clamped to 16. */
        expect(LIN, LIN, i, 0, 1, 3.5f, 1, en, i > 0 ? (levels[i] < 3.5f ? levels[i] : 3.5f) : 1.0f, __LINE__);
        expect(LIN, LIN, i, 0, 1, 64.0f, 1, en, levels[i], __LINE__);
    }
    expect(LIN, LIN, 1, 0, 1, 1.5f, 1, VK_TRUE, 1.5f, __LINE__);
    expect(LIN, LIN, 0, 16, 1, 3.5f, 1, VK_TRUE, 3.5f, __LINE__);
    expect(LIN, LIN, 0, 16, 1, 1000.0f, 1, VK_TRUE, 16.0f, __LINE__);
    for (j = 0; j < 3; ++j) {
        expect(LIN, LIN, bad_codes[j], 0, 1, 16.0f, 1, OFF, __LINE__);
        expect(LIN, LIN, bad_codes[j], 0, 1, 16.0f, 0, OFF, __LINE__);
        expect(LIN, LIN, bad_codes[j], 16, 1, 16.0f, 1, OFF, __LINE__);
    }

    /* Null sampler is a no-op. */
    rsx_vk_anisotropy_apply(NULL, 7, 16, 1, 16.0f, 1);
    CHECK(1, 0);

    /* Parser. */
    for (i = 0; i < 7; ++i) {
        v = 0xDEADBEEFu;
        CHECK(rsx_vk_anisotropy_parse_override(good_text[i], &v) == 1 && v == good_val[i], i);
    }
    for (i = 0; i < sizeof(bad_text) / sizeof(bad_text[0]); ++i) {
        v = 0xDEADBEEFu;
        CHECK(rsx_vk_anisotropy_parse_override(bad_text[i], &v) == 0 && v == 0xDEADBEEFu, i);
    }
    v = 0xDEADBEEFu;
    CHECK(rsx_vk_anisotropy_parse_override(NULL, &v) == 1 && v == 0, 0);
    CHECK(rsx_vk_anisotropy_parse_override(NULL, NULL) == 0, 0);
    CHECK(rsx_vk_anisotropy_parse_override("8", NULL) == 0, 0);

    printf("rsx_vulkan_anisotropy: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

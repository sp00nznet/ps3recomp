/* Synthetic GPU test for the rsx_fsr3_vk consumer, not a game test.
 * Usage: test_vulkan_fsr3_consumer OUTPUT_PREFIX [FRAMES>=24]
 *
 * A procedural scene (static high-frequency background, one moving textured
 * square) is rendered on the CPU at 1280x720 with the FSR jitter sequence and
 * upscaled to 1920x1080 by rsx_fsr3_evaluate. The same renderer contract as
 * the game path: previous-minus-current unjittered motion in render pixels,
 * jitter in render pixels (+x right, +y down), inputs SHADER_READ_ONLY,
 * output GENERAL. Checks: argument rejection, finite output, and that the
 * documented conventions beat flipped motion and flipped jitter and a plain
 * bilinear upscale on PSNR against the analytic ground truth. */
#include "../libs/video/rsx_fsr3_vk.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t OW = 1920, OH = 1080;
constexpr double PI = 3.14159265358979323846;
int failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
void vk(VkResult r, const char *what)
{
    if (r != VK_SUCCESS) { std::fprintf(stderr, "Vulkan %s failed (%d)\n", what, (int)r); std::exit(2); }
}

uint16_t to_half(float f)
{
    uint32_t x; std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = (int32_t)((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) {
        if (e < -10) return (uint16_t)sign;
        m |= 0x800000u;
        const uint32_t shift = (uint32_t)(14 - e);
        uint32_t h = m >> shift;
        const uint32_t rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1))) ++h;
        return (uint16_t)(sign | h);
    }
    if (e >= 31) return (uint16_t)(sign | 0x7c00u);
    uint32_t h = sign | ((uint32_t)e << 10) | (m >> 13);
    const uint32_t rem = m & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) ++h;
    return (uint16_t)h;
}
float from_half(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    uint32_t x;
    if (e == 0) {
        if (!m) x = sign;
        else { int s = -1; uint32_t mm = m; do { ++s; mm <<= 1; } while (!(mm & 0x400)); x = sign | ((uint32_t)(127 - 15 - s) << 23) | ((mm & 0x3ff) << 13); }
    } else if (e == 31) x = sign | 0x7f800000u | (m << 13);
    else x = sign | ((e - 15 + 127) << 23) | (m << 13);
    float f; std::memcpy(&f, &x, 4); return f;
}

/* Scene in output-pixel coordinates (y down). */
struct Scene { double ox, oy, vx, vy, size; };
double background(double x, double y)
{
    return 0.5 + 0.22 * std::sin(2 * PI * x / 4.3) * std::sin(2 * PI * y / 3.7) +
           0.18 * ((((int)std::floor(x / 23.0) + (int)std::floor(y / 23.0)) & 1) ? 1 : -1);
}
double object(double x, double y)
{
    return 0.5 + 0.35 * std::sin(2 * PI * (x + 0.5 * y) / 5.1);
}
bool inside(const Scene &s, uint32_t frame, double x, double y, double *lx, double *ly)
{
    *lx = x - (s.ox + s.vx * frame); *ly = y - (s.oy + s.vy * frame);
    return *lx >= 0 && *ly >= 0 && *lx < s.size && *ly < s.size;
}
/* Box-filtered (4x4 supersampled) radiance over one pixel footprint. */
void shade(const Scene &s, uint32_t frame, double cx, double cy, double footprint,
           float *rgb, float *depth, bool *on_object)
{
    double sum[3] = {0, 0, 0}; int hits = 0;
    for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) {
        const double x = cx + ((i + 0.5) / 4 - 0.5) * footprint, y = cy + ((j + 0.5) / 4 - 0.5) * footprint;
        double lx, ly;
        if (inside(s, frame, x, y, &lx, &ly)) { const double v = object(lx, ly); sum[0] += v; sum[1] += 0.6 * v; sum[2] += 0.2; ++hits; }
        else { const double v = background(x, y); sum[0] += 0.3 * v; sum[1] += v; sum[2] += 0.8 * v; }
    }
    for (int c = 0; c < 3; ++c) rgb[c] = (float)(sum[c] / 16);
    double lx, ly;
    *on_object = inside(s, frame, cx, cy, &lx, &ly);
    *depth = *on_object ? 0.3f : 0.9f;
    (void)hits;
}

struct Gpu {
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
    uint32_t family = 0; VkCommandPool pool{}; VkCommandBuffer cmd{}; VkFence fence{};
    VkQueryPool queries{}; double timestamp_ns = 1;
    VkPhysicalDeviceMemoryProperties memory{};
};
struct Image { VkImage image{}; VkDeviceMemory memory{}; VkFormat format{}; uint32_t w = 0, h = 0; VkImageUsageFlags usage = 0; };
struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; void *map = nullptr; VkDeviceSize size = 0; };

uint32_t memory_type(const Gpu &g, uint32_t bits, VkMemoryPropertyFlags flags)
{
    for (uint32_t i = 0; i < g.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g.memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    std::fprintf(stderr, "no memory type\n"); std::exit(2);
}
Image make_image(const Gpu &g, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage)
{
    Image im; im.format = f; im.w = w; im.h = h; im.usage = usage;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = f; ci.extent = {w, h, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    vk(vkCreateImage(g.device, &ci, nullptr, &im.image), "create image");
    VkMemoryRequirements r; vkGetImageMemoryRequirements(g.device, im.image, &r);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size; ai.memoryTypeIndex = memory_type(g, r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk(vkAllocateMemory(g.device, &ai, nullptr, &im.memory), "allocate image");
    vk(vkBindImageMemory(g.device, im.image, im.memory, 0), "bind image");
    return im;
}
Buffer make_buffer(const Gpu &g, VkDeviceSize size, VkBufferUsageFlags usage)
{
    Buffer b; b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = size; ci.usage = usage;
    vk(vkCreateBuffer(g.device, &ci, nullptr, &b.buffer), "create buffer");
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(g.device, b.buffer, &r);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = memory_type(g, r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk(vkAllocateMemory(g.device, &ai, nullptr, &b.memory), "allocate buffer");
    vk(vkBindBufferMemory(g.device, b.buffer, b.memory, 0), "bind buffer");
    vk(vkMapMemory(g.device, b.memory, 0, size, 0, &b.map), "map buffer");
    return b;
}
void barrier(VkCommandBuffer c, const Image &im, VkImageLayout from, VkImageLayout to,
             VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from; b.newLayout = to; b.srcAccessMask = src; b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = im.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(c, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}
rsx_fsr3_image frame_image(const Image &im, VkImageLayout layout)
{
    return rsx_fsr3_image{im.image, im.format, im.w, im.h, im.usage, layout};
}

Gpu open_gpu()
{
    Gpu g;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_2;
    app.pApplicationName = "test_vulkan_fsr3_consumer";
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ic.pApplicationInfo = &app;
    vk(vkCreateInstance(&ic, nullptr, &g.instance), "create instance");
    uint32_t n = 0; vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n); vkEnumeratePhysicalDevices(g.instance, &n, devices.data());
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p);
        if (!g.physical || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) g.physical = d;
    }
    if (!g.physical) { std::fprintf(stderr, "no Vulkan device\n"); std::exit(2); }
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(g.physical, &p);
    g.timestamp_ns = p.limits.timestampPeriod;
    std::printf("{\"device\": \"%s\", \"api\": \"%u.%u\"}\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(g.physical, &g.memory);
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n); vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &n, families.data());
    for (g.family = 0; g.family < n; ++g.family)
        if ((families[g.family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) break;
    if (g.family == n) { std::fprintf(stderr, "no graphics+compute queue\n"); std::exit(2); }
    rsx_fsr3_requirements req{};
    if (rsx_fsr3_device_requirements(g.physical, &req) != RSX_FSR3_OK) { std::fprintf(stderr, "device lacks FSR3 requirements\n"); std::exit(3); }
    const float priority = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueFamilyIndex = g.family; q.queueCount = 1; q.pQueuePriorities = &priority;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &q; dc.pEnabledFeatures = &req.required;
    vk(vkCreateDevice(g.physical, &dc, nullptr, &g.device), "create device");
    vkGetDeviceQueue(g.device, g.family, 0, &g.queue);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pc.queueFamilyIndex = g.family;
    vk(vkCreateCommandPool(g.device, &pc, nullptr, &g.pool), "create pool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = g.pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
    vk(vkAllocateCommandBuffers(g.device, &ca, &g.cmd), "allocate command buffer");
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vk(vkCreateFence(g.device, &fc, nullptr, &g.fence), "create fence");
    VkQueryPoolCreateInfo qc{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qc.queryType = VK_QUERY_TYPE_TIMESTAMP; qc.queryCount = 2;
    vk(vkCreateQueryPool(g.device, &qc, nullptr, &g.queries), "create query pool");
    return g;
}

void submit(Gpu &g)
{
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &g.cmd;
    vk(vkQueueSubmit(g.queue, 1, &si, g.fence), "submit");
    vk(vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX), "wait");
    vk(vkResetFences(g.device, 1, &g.fence), "reset fence");
}

struct Result { double psnr_static = 0, psnr_object = 0, gpu_ms = 0; bool finite = true; std::vector<float> rgb; };

double psnr(const std::vector<float> &a, const std::vector<double> &truth, const std::vector<uint8_t> &mask, uint8_t want)
{
    double se = 0; size_t n = 0;
    for (size_t p = 0; p < mask.size(); ++p) if (mask[p] == want)
        for (int c = 0; c < 3; ++c) { const double d = a[p * 3 + c] - truth[p * 3 + c]; se += d * d; ++n; }
    return n ? 10 * std::log10(1.0 / (se / (double)n + 1e-12)) : 0;
}

/* mv_sign / jitter_sign = +1 for the documented conventions. */
/* One set of images for every run: the backend keeps views of the last 3
 * evaluations' images alive, so they must outlive those evaluations. */
struct Targets { Image color, depth, motion, out; Buffer up, down; };
Targets make_targets(const Gpu &g, uint32_t RW, uint32_t RH)
{
    Targets t;
    t.color = make_image(g, VK_FORMAT_R16G16B16A16_SFLOAT, RW, RH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    t.depth = make_image(g, VK_FORMAT_R32_SFLOAT, RW, RH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    t.motion = make_image(g, VK_FORMAT_R32G32_SFLOAT, RW, RH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    t.out = make_image(g, VK_FORMAT_R16G16B16A16_SFLOAT, OW, OH, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    t.up = make_buffer(g, (VkDeviceSize)RW * RH * 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    t.down = make_buffer(g, (VkDeviceSize)OW * OH * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    return t;
}
void destroy_targets(const Gpu &g, Targets &t)
{
    for (Image *im : {&t.color, &t.depth, &t.motion, &t.out}) { vkDestroyImage(g.device, im->image, nullptr); vkFreeMemory(g.device, im->memory, nullptr); }
    for (Buffer *b : {&t.up, &t.down}) { vkDestroyBuffer(g.device, b->buffer, nullptr); vkFreeMemory(g.device, b->memory, nullptr); }
}

Result run(Gpu &g, rsx_fsr3_vk *fsr, Targets &t, uint64_t generation, uint32_t RW, uint32_t RH, uint32_t frames,
           double mv_sign, double jitter_sign, const Scene &scene, uint64_t *serial)
{
    Image &color = t.color, &depth = t.depth, &motion = t.motion, &out = t.out;
    Buffer &up = t.up, &down = t.down;
    const VkDeviceSize csz = (VkDeviceSize)RW * RH * 8, dsz = (VkDeviceSize)RW * RH * 4;
    const double sx = (double)OW / RW, sy = (double)OH / RH;
    Result res; double gpu_ns = 0; uint32_t timed = 0;
    for (uint32_t k = 0; k < frames; ++k) {
        float jx = 0, jy = 0;
        rsx_fsr3_jitter(RW, OW, k, &jx, &jy, nullptr);
        uint16_t *cp = (uint16_t *)up.map; float *dp = (float *)((uint8_t *)up.map + csz); float *mp = (float *)((uint8_t *)up.map + csz + dsz);
        for (uint32_t y = 0; y < RH; ++y) for (uint32_t x = 0; x < RW; ++x) {
            /* The projection moves the scene by +jitter, so a pixel centre sees centre - jitter. */
            const double cx = (x + 0.5 - jitter_sign * jx) * sx, cy = (y + 0.5 - jitter_sign * jy) * sy;
            float rgb[3], d; bool on;
            shade(scene, k, cx, cy, sx, rgb, &d, &on);
            const size_t p = (size_t)y * RW + x;
            for (int c = 0; c < 3; ++c) cp[p * 4 + c] = to_half(rgb[c]);
            cp[p * 4 + 3] = to_half(1.0f);
            dp[p] = d;
            mp[p * 2 + 0] = on ? (float)(mv_sign * -scene.vx / sx) : 0.0f;
            mp[p * 2 + 1] = on ? (float)(mv_sign * -scene.vy / sy) : 0.0f;
        }
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk(vkBeginCommandBuffer(g.cmd, &bi), "begin");
        vkCmdResetQueryPool(g.cmd, g.queries, 0, 2);
        const VkImageLayout in0 = k ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        for (const Image *im : {&color, &depth, &motion})
            barrier(g.cmd, *im, in0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {RW, RH, 1};
        copy.bufferOffset = 0; vkCmdCopyBufferToImage(g.cmd, up.buffer, color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        copy.bufferOffset = csz; vkCmdCopyBufferToImage(g.cmd, up.buffer, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        copy.bufferOffset = csz + dsz; vkCmdCopyBufferToImage(g.cmd, up.buffer, motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        for (const Image *im : {&color, &depth, &motion})
            barrier(g.cmd, *im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(g.cmd, out, k ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        rsx_fsr3_frame f{};
        f.config_generation = generation; f.frame_serial = f.motion_frame_serial = ++*serial;
        f.motion_provenance = RSX_FSR3_MOTION_CAMERA_AND_OBJECT_UNJITTERED; f.render_width = RW; f.render_height = RH;
        f.color = frame_image(color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        f.depth = frame_image(depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        f.motion = frame_image(motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        f.output = frame_image(out, VK_IMAGE_LAYOUT_GENERAL);
        f.jitter_x = jx; f.jitter_y = jy; f.delta_milliseconds = 16.6667f; f.pre_exposure = 1;
        f.camera_near = 0.1f; f.camera_far = 100.0f; f.vertical_fov_radians = 1.0f; f.sharpness = 0; f.reset = k == 0;
        vkCmdWriteTimestamp(g.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.queries, 0);
        const rsx_fsr3_status s = rsx_fsr3_evaluate(fsr, g.cmd, &f);
        vkCmdWriteTimestamp(g.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.queries, 1);
        if (s != RSX_FSR3_OK) { std::fprintf(stderr, "evaluate frame %u -> %d\n", k, (int)s); std::exit(4); }
        barrier(g.cmd, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy back{}; back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; back.imageExtent = {OW, OH, 1};
        vkCmdCopyImageToBuffer(g.cmd, out.image, VK_IMAGE_LAYOUT_GENERAL, down.buffer, 1, &back);
        vk(vkEndCommandBuffer(g.cmd), "end");
        submit(g);
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(g.device, g.queries, 0, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && k >= 4) {
            gpu_ns += (double)(ts[1] - ts[0]) * g.timestamp_ns; ++timed;
        }
    }
    res.gpu_ms = timed ? gpu_ns / timed / 1e6 : 0;
    const uint16_t *hp = (const uint16_t *)down.map;
    res.rgb.resize((size_t)OW * OH * 3);
    for (size_t p = 0; p < (size_t)OW * OH; ++p) for (int c = 0; c < 3; ++c) {
        const float v = from_half(hp[p * 4 + c]); res.rgb[p * 3 + c] = v; if (!std::isfinite(v)) res.finite = false;
    }
    return res;
}

void write_ppm(const std::string &path, const float *rgb, uint32_t w, uint32_t h, size_t stride = 3)
{
    FILE *f = std::fopen(path.c_str(), "wb"); if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> row((size_t)w * 3);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) for (int c = 0; c < 3; ++c) {
            const double v = rgb[((size_t)y * w + x) * stride + c];
            row[(size_t)x * 3 + c] = (uint8_t)std::lround(255 * (v < 0 ? 0 : v > 1 ? 1 : v));
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s OUTPUT_PREFIX [FRAMES>=24]\n", argv[0]); return 2; }
    const std::string prefix = argv[1];
    const uint32_t frames = argc > 2 ? (uint32_t)std::atoi(argv[2]) : 48;
    if (frames < 24) return 2;
    Gpu g = open_gpu();
    uint32_t RW = 0, RH = 0;
    check(rsx_fsr3_render_size(OW, OH, RSX_FSR3_QUALITY, &RW, &RH) == RSX_FSR3_OK && RW == 1280 && RH == 720,
          "quality render size is 1280x720");
    rsx_fsr3_device dev{g.physical, g.device, vkGetDeviceProcAddr, 0};
    rsx_fsr3_vk *fsr = nullptr; rsx_fsr3_report report{};
    check(rsx_fsr3_create(&dev, &fsr, &report) == RSX_FSR3_OK && fsr, "create");
    if (!fsr) return 1;
    rsx_fsr3_config cfg{1, RW, RH, OW, OH, RSX_FSR3_COLOR_LDR, 0, 0, 0};
    check(rsx_fsr3_configure(fsr, &cfg) == RSX_FSR3_OK, "configure");
    check(rsx_fsr3_configure(fsr, &cfg) == RSX_FSR3_BAD_STATE, "second configure refused");
    rsx_fsr3_get_report(fsr, &report);

    /* Argument rejection, nothing recorded: begin/end an empty command buffer. */
    {
        Image c = make_image(g, VK_FORMAT_R16G16B16A16_SFLOAT, RW, RH, VK_IMAGE_USAGE_SAMPLED_BIT);
        Image o = make_image(g, VK_FORMAT_R16G16B16A16_SFLOAT, OW, OH, VK_IMAGE_USAGE_STORAGE_BIT);
        Image d = make_image(g, VK_FORMAT_R32_SFLOAT, RW, RH, VK_IMAGE_USAGE_SAMPLED_BIT);
        Image m = make_image(g, VK_FORMAT_R32G32_SFLOAT, RW, RH, VK_IMAGE_USAGE_SAMPLED_BIT);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vk(vkBeginCommandBuffer(g.cmd, &bi), "begin");
        rsx_fsr3_frame f{};
        f.config_generation = 1; f.frame_serial = f.motion_frame_serial = 1; f.motion_provenance = RSX_FSR3_MOTION_CAMERA_AND_OBJECT_UNJITTERED;
        f.render_width = RW; f.render_height = RH; f.color = frame_image(c, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        f.depth = frame_image(d, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); f.motion = frame_image(m, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        f.output = frame_image(o, VK_IMAGE_LAYOUT_GENERAL); f.delta_milliseconds = 16; f.pre_exposure = 1;
        f.camera_near = 0.1f; f.camera_far = 100; f.vertical_fov_radians = 1; f.reset = 0;
        check(rsx_fsr3_evaluate(fsr, g.cmd, &f) == RSX_FSR3_BAD_ARGUMENT, "first frame without reset refused");
        f.reset = 1; f.output.declared_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        check(rsx_fsr3_evaluate(fsr, g.cmd, &f) == RSX_FSR3_BAD_ARGUMENT, "output not GENERAL refused");
        f.output.declared_layout = VK_IMAGE_LAYOUT_GENERAL; f.output.declared_usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        check(rsx_fsr3_evaluate(fsr, g.cmd, &f) == RSX_FSR3_BAD_ARGUMENT, "output without storage usage refused");
        f.output = frame_image(o, VK_IMAGE_LAYOUT_GENERAL); f.config_generation = 2;
        check(rsx_fsr3_evaluate(fsr, g.cmd, &f) == RSX_FSR3_BAD_ARGUMENT, "stale generation refused");
        f.config_generation = 1; f.motion_provenance = 0;
        check(rsx_fsr3_evaluate(fsr, g.cmd, &f) == RSX_FSR3_BAD_ARGUMENT, "missing motion provenance refused");
        f.motion_provenance = RSX_FSR3_MOTION_CAMERA_AND_OBJECT_UNJITTERED; f.jitter_x = 1.5f;
        check(rsx_fsr3_evaluate(fsr, g.cmd, &f) == RSX_FSR3_BAD_ARGUMENT, "jitter beyond one pixel refused");
        f.jitter_x = 0; f.render_width = RW + 1;
        check(rsx_fsr3_evaluate(fsr, g.cmd, &f) == RSX_FSR3_BAD_ARGUMENT, "render extent above configured maximum refused");
        vk(vkEndCommandBuffer(g.cmd), "end");
        vk(vkResetCommandBuffer(g.cmd, 0), "reset");
        for (Image *im : {&c, &o, &d, &m}) { vkDestroyImage(g.device, im->image, nullptr); vkFreeMemory(g.device, im->memory, nullptr); }
    }

    const Scene scene{560, 300, 3.0, 1.5, 420};
    uint64_t serial = 100;
    /* Documented conventions, then motion flipped, then jitter flipped. Each
     * starts with reset, so histories never mix. */
    Targets targets = make_targets(g, RW, RH);
    Result good = run(g, fsr, targets, 1, RW, RH, frames, +1, +1, scene, &serial);
    Result bad_mv = run(g, fsr, targets, 1, RW, RH, frames, -1, +1, scene, &serial);
    Result bad_jitter = run(g, fsr, targets, 1, RW, RH, frames, +1, -1, scene, &serial);

    /* Analytic truth for the last frame; object interior / static background
     * masks keep 12 px away from edges and from the object's recent path. */
    const uint32_t last = frames - 1;
    std::vector<double> truth((size_t)OW * OH * 3);
    std::vector<float> bilinear((size_t)OW * OH * 3);
    std::vector<uint8_t> mask((size_t)OW * OH, 0);
    std::vector<float> render((size_t)RW * RH * 3);
    const double sx = (double)OW / RW, sy = (double)OH / RH;
    for (uint32_t y = 0; y < RH; ++y) for (uint32_t x = 0; x < RW; ++x) {
        float rgb[3], d; bool on; shade(scene, last, (x + 0.5) * sx, (y + 0.5) * sy, sx, rgb, &d, &on);
        for (int c = 0; c < 3; ++c) render[((size_t)y * RW + x) * 3 + c] = rgb[c];
    }
    for (uint32_t y = 0; y < OH; ++y) for (uint32_t x = 0; x < OW; ++x) {
        const size_t p = (size_t)y * OW + x;
        float rgb[3], d; bool on; shade(scene, last, x + 0.5, y + 0.5, 1.0, rgb, &d, &on);
        for (int c = 0; c < 3; ++c) truth[p * 3 + c] = rgb[c];
        const double rx = (x + 0.5) / sx - 0.5, ry = (y + 0.5) / sy - 0.5;
        const int x0 = (int)std::floor(rx), y0 = (int)std::floor(ry);
        const double fx = rx - x0, fy = ry - y0;
        auto at = [&](int xx, int yy, int c) { xx = xx < 0 ? 0 : xx >= (int)RW ? RW - 1 : xx; yy = yy < 0 ? 0 : yy >= (int)RH ? RH - 1 : yy; return render[((size_t)yy * RW + xx) * 3 + c]; };
        for (int c = 0; c < 3; ++c)
            bilinear[p * 3 + c] = (float)((1 - fy) * ((1 - fx) * at(x0, y0, c) + fx * at(x0 + 1, y0, c)) + fy * ((1 - fx) * at(x0, y0 + 1, c) + fx * at(x0 + 1, y0 + 1, c)));
        const double ox = scene.ox + scene.vx * last, oy = scene.oy + scene.vy * last;
        const bool interior = x >= ox + 12 && y >= oy + 12 && x < ox + scene.size - 12 && y < oy + scene.size - 12;
        const double tx = scene.ox + scene.vx * (last > 20 ? last - 20 : 0), ty = scene.oy + scene.vy * (last > 20 ? last - 20 : 0);
        const bool near_path = x + 12 >= std::fmin(tx, ox) && y + 12 >= std::fmin(ty, oy) &&
                               x < std::fmax(tx, ox) + scene.size + 12 && y < std::fmax(ty, oy) + scene.size + 12;
        const bool border = x < 16 || y < 16 || x >= OW - 16 || y >= OH - 16;
        mask[p] = border ? 0 : interior ? 2 : near_path ? 0 : 1;
    }
    for (Result *r : {&good, &bad_mv, &bad_jitter}) { r->psnr_static = psnr(r->rgb, truth, mask, 1); r->psnr_object = psnr(r->rgb, truth, mask, 2); }
    const double bil_static = psnr(bilinear, truth, mask, 1), bil_object = psnr(bilinear, truth, mask, 2);
    std::printf("{\"render\": [%u, %u], \"output\": [%u, %u], \"frames\": %u, \"fsr_vram_mb\": %.1f, \"gpu_ms_per_evaluate\": %.3f,\n"
                " \"psnr_static_db\": {\"fsr\": %.2f, \"mv_flipped\": %.2f, \"jitter_flipped\": %.2f, \"bilinear\": %.2f},\n"
                " \"psnr_object_db\": {\"fsr\": %.2f, \"mv_flipped\": %.2f, \"jitter_flipped\": %.2f, \"bilinear\": %.2f}}\n",
                RW, RH, OW, OH, frames, report.gpu_memory_bytes / 1048576.0, good.gpu_ms,
                good.psnr_static, bad_mv.psnr_static, bad_jitter.psnr_static, bil_static,
                good.psnr_object, bad_mv.psnr_object, bad_jitter.psnr_object, bil_object);
    check(good.finite && bad_mv.finite && bad_jitter.finite, "all outputs finite");
    check(good.psnr_object > bad_mv.psnr_object + 1.0, "documented motion sign beats flipped motion on the moving object");
    check(good.psnr_static > bad_jitter.psnr_static + 0.5, "documented jitter sign beats flipped jitter on the static background");
    check(good.psnr_static > bil_static, "FSR beats bilinear on the static background");
    write_ppm(prefix + "-fsr.ppm", good.rgb.data(), OW, OH);
    write_ppm(prefix + "-mv-flipped.ppm", bad_mv.rgb.data(), OW, OH);
    write_ppm(prefix + "-bilinear.ppm", bilinear.data(), OW, OH);
    std::vector<float> tf(truth.begin(), truth.end());
    write_ppm(prefix + "-truth.ppm", tf.data(), OW, OH);
    rsx_fsr3_report final_report{}; rsx_fsr3_get_report(fsr, &final_report);
    check(final_report.evaluation_count == 3ull * frames, "evaluation count");
    vkDeviceWaitIdle(g.device);
    rsx_fsr3_destroy(fsr);
    destroy_targets(g, targets);
    vkDestroyQueryPool(g.device, g.queries, nullptr); vkDestroyFence(g.device, g.fence, nullptr);
    vkDestroyCommandPool(g.device, g.pool, nullptr); vkDestroyDevice(g.device, nullptr); vkDestroyInstance(g.instance, nullptr);
    std::printf("{\"failures\": %d}\n", failures);
    return failures ? 1 : 0;
}

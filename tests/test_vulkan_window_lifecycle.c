/* CPU regression of the actual backend lifecycle functions with controlled
 * Vulkan/SDL responses. This does not claim GPU presentation validation. */
#include "../libs/video/rsx_vulkan_backend.c"
static int failures, checks, creates, destroys, window_destroys, quit_event;
static int drawable_w, drawable_h;
static Uint32 window_flags;
static VkResult caps_result = VK_SUCCESS;
static VkSurfaceCapabilitiesKHR test_caps;
static void check(int ok, const char* name) {
    ++checks;
    if (!ok) { fprintf(stderr, "FAIL %s\n", name); ++failures; }
}
Uint32 SDLCALL SDL_GetWindowFlags(SDL_Window* window) { (void)window; return window_flags; }
void SDLCALL SDL_Vulkan_GetDrawableSize(SDL_Window* window, int* w, int* h) {
    (void)window; *w = drawable_w; *h = drawable_h;
}
void SDLCALL SDL_DestroyWindow(SDL_Window* window) { (void)window; ++window_destroys; }
void SDLCALL SDL_QuitSubSystem(Uint32 flags) { (void)flags; }
int SDLCALL SDL_PollEvent(SDL_Event* event) {
    if (!quit_event) return 0;
    quit_event = 0; event->type = SDL_QUIT; return 1;
}
static VKAPI_ATTR VkResult VKAPI_CALL idle(VkDevice d) { (void)d; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL caps(VkPhysicalDevice d, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR* out) {
    (void)d; (void)s; *out = test_caps; return caps_result;
}
static VKAPI_ATTR VkResult VKAPI_CALL formats(VkPhysicalDevice d, VkSurfaceKHR s, uint32_t* n, VkSurfaceFormatKHR* out) {
    (void)d; (void)s; *n = 1;
    if (out) *out = (VkSurfaceFormatKHR){ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL props(VkPhysicalDevice d, VkFormat f, VkFormatProperties* out) {
    (void)d; (void)f; memset(out, 0, sizeof *out); out->optimalTilingFeatures = VK_FORMAT_FEATURE_BLIT_DST_BIT;
}
static VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice d, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    (void)d; (void)a; ++creates;
    check(info->imageExtent.width && info->imageExtent.height, "never create zero extent");
    *out = (VkSwapchainKHR)(uintptr_t)(100 + creates); return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks* a) {
    (void)d; (void)s; (void)a; ++destroys;
}
static VKAPI_ATTR VkResult VKAPI_CALL images(VkDevice d, VkSwapchainKHR s, uint32_t* n, VkImage* out) {
    (void)d; (void)s; *n = 3;
    if (out) for (int i = 0; i < 3; ++i) out[i] = (VkImage)(uintptr_t)(200 + i);
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL semaphore(VkDevice d, const VkSemaphoreCreateInfo* i, const VkAllocationCallbacks* a, VkSemaphore* out) {
    (void)d; (void)i; (void)a; *out = (VkSemaphore)(uintptr_t)300; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_semaphore(VkDevice d, VkSemaphore s, const VkAllocationCallbacks* a) {
    (void)d; (void)s; (void)a;
}
int main(void) {
    pvkDeviceWaitIdle = idle; pvkGetPhysicalDeviceSurfaceCapabilitiesKHR = caps;
    pvkGetPhysicalDeviceSurfaceFormatsKHR = formats; pvkGetPhysicalDeviceFormatProperties = props;
    pvkCreateSwapchainKHR = create; pvkDestroySwapchainKHR = destroy;
    pvkGetSwapchainImagesKHR = images; pvkCreateSemaphore = semaphore; pvkDestroySemaphore = destroy_semaphore;
    s_vk.windowed = 1; s_vk.window = (SDL_Window*)(uintptr_t)1;
    s_vk.device = (VkDevice)(uintptr_t)2; s_vk.surface = (VkSurfaceKHR)(uintptr_t)3;
    s_vk.swapchain = (VkSwapchainKHR)(uintptr_t)4; s_vk.sem_acquire = (VkSemaphore)(uintptr_t)5;
    s_vk.successful_window_presents = 9; s_vk.swapchain_dirty = 1;
    test_caps.minImageCount = 2; test_caps.maxImageCount = 8;
    test_caps.minImageExtent = (VkExtent2D){1, 1}; test_caps.maxImageExtent = (VkExtent2D){4096, 4096};
    test_caps.supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    test_caps.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    test_caps.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    check(!vk_prepare_window_present(), "zero surface defers");
    check(rsx_vulkan_backend_present_deferred() && rsx_vulkan_backend_window_active(), "zero surface retains active window");
    check(!creates && !destroys && s_vk.swapchain == (VkSwapchainKHR)(uintptr_t)4, "zero surface preserves swapchain");
    check(!vk_prepare_window_present() && !window_destroys, "repeated zero remains recoverable");
    quit_event = 1; check(rsx_vulkan_backend_pump_messages() == -1, "close event handled during zero extent");
    test_caps.currentExtent = (VkExtent2D){1280, 720};
    check(vk_prepare_window_present(), "restored surface ready");
    check(creates == 1 && destroys == 1 && !s_vk.swapchain_dirty, "restoration rebuilds once");
    check(!rsx_vulkan_backend_present_deferred(), "restored status clears deferral");
    window_flags = SDL_WINDOW_MINIMIZED;
    check(!vk_prepare_window_present() && creates == 1, "minimized window avoids acquire or recreate");
    window_flags = 0;
    check(vk_prepare_window_present() && creates == 2, "unminimized window rebuilds");
    test_caps.currentExtent = (VkExtent2D){UINT32_MAX, UINT32_MAX};
    drawable_w = 0; drawable_h = 0; s_vk.swapchain_dirty = 1;
    check(!vk_prepare_window_present() && creates == 2, "zero drawable is not clamped to minimum");
    drawable_w = 640; drawable_h = 480;
    check(vk_prepare_window_present() && s_vk.sc_extent.width == 640 && s_vk.sc_extent.height == 480, "restored drawable supplies extent");
    vk_swapchain_lost();
    check(rsx_vulkan_backend_present_deferred(), "outdated acquisition still defers after recreation");
    check(vk_prepare_window_present() && !rsx_vulkan_backend_present_deferred(), "next attempt clears outdated deferral");
    caps_result = VK_ERROR_SURFACE_LOST_KHR; s_vk.swapchain_dirty = 1;
    check(!vk_prepare_window_present(), "permanent surface failure not ready");
    check(!rsx_vulkan_backend_present_deferred() && !rsx_vulkan_backend_window_active() && window_destroys == 1,
          "permanent failure never disguised as deferral");
    check(rsx_vulkan_backend_successful_presents() == 9, "no successful presents fabricated");
    printf("Vulkan window lifecycle: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

/* Optional capability inspection only: no SDK loading, device requirements,
 * shader changes or per-frame overhead. Vulkan 1.0 retains unknown features. */
static rsx_render_capabilities s_advanced;

static void vk_advanced_reset(void)
{
    memset(&s_advanced, 0, sizeof s_advanced);
    s_advanced.acceleration_structure = -1;
    s_advanced.ray_tracing_pipeline = -1;
    s_advanced.ray_query = -1;
    s_advanced.buffer_device_address = -1;
    s_advanced.deferred_host_operations = -1;
}

/* Unlike device selection's boolean helper, distinguish an enumeration failure
 * from a device that really lacks an extension. Retry an unstable list once. */
static int vk_advanced_extensions(VkExtensionProperties** out, u32* count)
{
    *out = NULL; *count = 0;
    for (unsigned retry = 0; retry < 2; ++retry) {
        u32 n = 0;
        if (pvkEnumerateDeviceExtensionProperties(s_vk.phys, NULL, &n, NULL) != VK_SUCCESS || n > 4096)
            return 0;
        if (!n) return 1;
        VkExtensionProperties* p = (VkExtensionProperties*)calloc(n, sizeof *p);
        if (!p) return 0;
        VkResult r = pvkEnumerateDeviceExtensionProperties(s_vk.phys, NULL, &n, p);
        if (r == VK_SUCCESS) { *out = p; *count = n; return 1; }
        free(p);
        if (r != VK_INCOMPLETE) return 0;
    }
    return 0;
}

static int vk_advanced_has(const VkExtensionProperties* p, u32 n, const char* name)
{
    for (u32 i = 0; i < n; ++i)
        if (strcmp(p[i].extensionName, name) == 0) return 1;
    return 0;
}

static void vk_advanced_probe(u32 instance_api)
{
    vk_advanced_reset();
    VkPhysicalDeviceProperties properties;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &properties);
    s_advanced.instance_api = instance_api;
    s_advanced.device_api = properties.apiVersion;
    s_advanced.vendor_id = properties.vendorID;
    s_advanced.device_id = properties.deviceID;
    VkExtensionProperties* p = NULL; u32 n = 0;
    if (!vk_advanced_extensions(&p, &n)) return;
    const int as = vk_advanced_has(p,n,"VK_KHR_acceleration_structure");
    const int rt = vk_advanced_has(p,n,"VK_KHR_ray_tracing_pipeline");
    const int rq = vk_advanced_has(p,n,"VK_KHR_ray_query");
    const int bda = vk_advanced_has(p,n,"VK_KHR_buffer_device_address");
    s_advanced.deferred_host_operations = vk_advanced_has(p,n,"VK_KHR_deferred_host_operations");
    free(p);
    if (!as) s_advanced.acceleration_structure = 0;
    if (!rt) s_advanced.ray_tracing_pipeline = 0;
    if (!rq) s_advanced.ray_query = 0;
    if (!bda && properties.apiVersion < VK_API_VERSION_1_2) s_advanced.buffer_device_address = 0;
    /* Do not call a promoted core function on the existing 1.0 instance. */
    if (instance_api < VK_API_VERSION_1_1 || properties.apiVersion < VK_API_VERSION_1_1) return;
    PFN_vkGetPhysicalDeviceFeatures2 features2 = (PFN_vkGetPhysicalDeviceFeatures2)
        pvkGetInstanceProcAddr(s_vk.instance,"vkGetPhysicalDeviceFeatures2");
    if (!features2) return;
    VkPhysicalDeviceFeatures2 f = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR a = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR t = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    VkPhysicalDeviceBufferDeviceAddressFeatures b = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
    if (as) { a.pNext = f.pNext; f.pNext = &a; }
    if (rt) { t.pNext = f.pNext; f.pNext = &t; }
    if (rq) { q.pNext = f.pNext; f.pNext = &q; }
    const int query_bda = bda || (instance_api >= VK_API_VERSION_1_2 && properties.apiVersion >= VK_API_VERSION_1_2);
    if (query_bda) { b.pNext = f.pNext; f.pNext = &b; }
    features2(s_vk.phys, &f);
    if (as) s_advanced.acceleration_structure = a.accelerationStructure ? 1 : 0;
    if (rt) s_advanced.ray_tracing_pipeline = t.rayTracingPipeline ? 1 : 0;
    if (rq) s_advanced.ray_query = q.rayQuery ? 1 : 0;
    if (query_bda)
        s_advanced.buffer_device_address = b.bufferDeviceAddress ? 1 : 0;
}

rsx_render_capabilities rsx_vulkan_backend_advanced_capabilities(void)
{
    /* Zero-initialized static storage before the first backend init must not
     * imply a negative physical-device query. */
    if (!s_advanced.initialized) {
        rsx_render_capabilities unknown = {0};
        unknown.acceleration_structure = unknown.ray_tracing_pipeline = -1;
        unknown.ray_query = unknown.buffer_device_address = unknown.deferred_host_operations = -1;
        return unknown;
    }
    return s_advanced;
}

int rsx_vulkan_backend_advanced_report(char* out, size_t capacity)
{
    const rsx_render_capabilities c = rsx_vulkan_backend_advanced_capabilities();
    if (!out && capacity) return -1;
    /* Logical feature enablement is distinct from a qualified gameplay mode. */
    int n = snprintf(out, capacity,
        "{\"schema\":1,\"initialized\":%s,\"instance_api\":%u,\"device_api\":%u,"
        "\"vendor_id\":%u,\"device_id\":%u,\"physical_device_features\":{"
        "\"acceleration_structure\":%d,\"ray_tracing_pipeline\":%d,\"ray_query\":%d,"
        "\"buffer_device_address\":%d,\"deferred_host_operations\":%d},"
        "\"advanced_logical_device_features_enabled\":%s,"
        "\"ray_tracing\":{\"available\":false,\"active\":false,\"reason\":\"world_scene_and_ray_pipeline_unimplemented\"},"
        "\"path_tracing\":{\"available\":false,\"active\":false,\"reason\":\"world_scene_materials_lights_and_integrator_unimplemented\"},"
        "\"dlss_sr\":{\"available\":false,\"active\":false,\"sdk_support_queried\":false,"
        "\"reason\":\"sdk_integration_and_qualified_temporal_inputs_missing\"},"
        "\"fallback\":\"existing_raster_settings\"}",
        c.initialized ? "true" : "false",c.instance_api,c.device_api,c.vendor_id,c.device_id,
        c.acceleration_structure,c.ray_tracing_pipeline,c.ray_query,c.buffer_device_address,c.deferred_host_operations,
        s_vk.rt_enabled ? "true" : "false");
    if (n < 0 || (capacity && (size_t)n >= capacity)) { if (out && capacity) out[0]=0; return -1; }
    return n;
}

/* Real device inspection and raster fallback, with deterministic unavailable
 * device/extension cases. No game inputs or NVIDIA SDK are required. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
#include <assert.h>
uint8_t* vm_base;
static unsigned checks;
static u32 fake_api=VK_API_VERSION_1_0;
static unsigned features2_calls, bda_queries;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while (0)

static VKAPI_ATTR VkResult VKAPI_CALL no_extensions(VkPhysicalDevice pd,const char* layer,u32* n,VkExtensionProperties* p)
{ (void)pd; (void)layer; (void)p; *n=0; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL failed_extensions(VkPhysicalDevice pd,const char* layer,u32* n,VkExtensionProperties* p)
{ (void)pd; (void)layer; (void)n; (void)p; return VK_ERROR_OUT_OF_HOST_MEMORY; }
static VKAPI_ATTR void VKAPI_CALL old_properties(VkPhysicalDevice pd,VkPhysicalDeviceProperties* p)
{ (void)pd; memset(p,0,sizeof *p); p->apiVersion=fake_api; }
static VKAPI_ATTR void VKAPI_CALL fake_features2(VkPhysicalDevice pd,VkPhysicalDeviceFeatures2* p)
{
    (void)pd; ++features2_calls;
    for (VkBaseOutStructure* s=(VkBaseOutStructure*)p->pNext;s;s=s->pNext) {
        CHECK(s->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
        ++bda_queries;
        ((VkPhysicalDeviceBufferDeviceAddressFeatures*)s)->bufferDeviceAddress=VK_TRUE;
    }
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_get_proc(VkInstance instance,const char* name)
{ (void)instance; CHECK(!strcmp(name,"vkGetPhysicalDeviceFeatures2")); return (PFN_vkVoidFunction)fake_features2; }

static void report_checks(void)
{
    char json[2048];
    int n=rsx_vulkan_backend_advanced_report(NULL,0);
    CHECK(n>0 && n<(int)sizeof json);
    CHECK(rsx_vulkan_backend_advanced_report(json,sizeof json)==n);
    CHECK((int)strlen(json)==n);
    CHECK(strstr(json,"\"available\":true")==NULL);
    CHECK(strstr(json,"\"active\":true")==NULL);
    CHECK(strstr(json,"\"sdk_support_queried\":false")!=NULL);
    CHECK(strstr(json,"\"fallback\":\"existing_raster_settings\"")!=NULL);
    CHECK(rsx_vulkan_backend_advanced_report(json,(size_t)n)==-1 && json[0]==0);
    CHECK(rsx_vulkan_backend_advanced_report(json,(size_t)n+1)==n);
    CHECK(rsx_vulkan_backend_advanced_report(NULL,1)==-1);
}

int main(int argc,char** argv)
{
    (void)argv;
    CHECK(rsx_vulkan_backend_advanced_capabilities().acceleration_structure==-1);
    report_checks();
    pvkGetPhysicalDeviceProperties=old_properties;
    pvkEnumerateDeviceExtensionProperties=no_extensions;
    vk_advanced_probe(VK_API_VERSION_1_0);
    CHECK(s_advanced.acceleration_structure==0 && s_advanced.ray_query==0);
    CHECK(s_advanced.ray_tracing_pipeline==0 && s_advanced.buffer_device_address==0);
    CHECK(s_advanced.deferred_host_operations==0);
    pvkGetInstanceProcAddr=fake_get_proc;
    vk_advanced_probe(VK_API_VERSION_1_1);
    CHECK(features2_calls==0); /* New instance, old physical device. */
    fake_api=VK_API_VERSION_1_2;
    vk_advanced_probe(VK_API_VERSION_1_1);
    CHECK(features2_calls==1 && bda_queries==0);
    CHECK(s_advanced.buffer_device_address==-1); /* 1.2 device alone is not a query route. */
    vk_advanced_probe(VK_API_VERSION_1_2);
    CHECK(features2_calls==2 && bda_queries==1);
    CHECK(s_advanced.buffer_device_address==1);
    pvkEnumerateDeviceExtensionProperties=failed_extensions;
    vk_advanced_probe(VK_API_VERSION_1_0);
    CHECK(s_advanced.acceleration_structure==-1 && s_advanced.buffer_device_address==-1);
    vk_advanced_reset();
    if (argc>1) { printf("PASS %u CPU checks\n",checks); return 0; }
    /* Explicit deterministic fixture settings, not inherited user options. */
#ifdef _WIN32
    _putenv_s("PS3RECOMP_VK_WINDOW","0"); _putenv_s("PS3RECOMP_VK_GUEST_PROGRAMS","0");
    _putenv_s("PS3RECOMP_RSX_ENGINE","vtable"); _putenv_s("PS3RECOMP_UPSCALER","off");
    _putenv_s("PS3RECOMP_ANTIALIASING","off"); _putenv_s("PS3RECOMP_ANISOTROPY","auto");
#else
    setenv("PS3RECOMP_VK_WINDOW","0",1); setenv("PS3RECOMP_VK_GUEST_PROGRAMS","0",1);
    setenv("PS3RECOMP_RSX_ENGINE","vtable",1); setenv("PS3RECOMP_UPSCALER","off",1);
    setenv("PS3RECOMP_ANTIALIASING","off",1); setenv("PS3RECOMP_ANISOTROPY","auto",1);
#endif
    CHECK(rsx_vulkan_backend_init(64,64,"advanced capability fallback test")==0);
    rsx_render_capabilities c=rsx_vulkan_backend_advanced_capabilities();
    CHECK(c.initialized==1 && c.device_api>=VK_API_VERSION_1_0);
    report_checks();
    char json[2048]; rsx_vulkan_backend_advanced_report(json,sizeof json); puts(json);
    vk_cb_clear(NULL,0xf0,0xff336699,1.0f,0);
    rsx_vulkan_backend_present();
    CHECK(rsx_vulkan_backend_readback_center()==0xff336699);
    rsx_vulkan_backend_shutdown();
    CHECK(rsx_vulkan_backend_advanced_capabilities().initialized==0);
    CHECK(rsx_vulkan_backend_advanced_capabilities().ray_query==-1);
    report_checks();
    printf("PASS %u checks, physical device query and raster readback\n",checks);
    return 0;
}

/* Optional FSR 3.1 world upscaler through rsx_fsr3_vk.dll (AMD FidelityFX SDK
 * 1.1.4 upscaler, MIT). It shares the DLSS world frame path (input images,
 * jitter, geometry-matched motion, expanded output); only configuration and
 * evaluation differ. PS3RECOMP_FSR3=1 with PS3RECOMP_FSR3_BRIDGE naming the
 * bridge by absolute path. When the DLSS bridge loads, DLSS is used instead.
 * PS3RECOMP_FSR3_MODE quality|balanced|performance|ultra-performance|native,
 * PS3RECOMP_FSR3_SHARPNESS 0..1 (FSR's own RCAS; 0 = off, the default),
 * PS3RECOMP_FSR3_REACTIVE=1 passes the unmatched-motion bias as FSR's
 * reactive mask (off by default, unmeasured). */
#if defined(_WIN32)
#include "rsx_fsr3_vk.h"
typedef rsx_fsr3_status (*vk_fsr3_requirements_fn)(VkPhysicalDevice,rsx_fsr3_requirements*);
typedef rsx_fsr3_status (*vk_fsr3_create_fn)(const rsx_fsr3_device*,rsx_fsr3_vk**,rsx_fsr3_report*);
typedef void (*vk_fsr3_destroy_fn)(rsx_fsr3_vk*);
typedef rsx_fsr3_status (*vk_fsr3_render_size_fn)(uint32_t,uint32_t,rsx_fsr3_quality,uint32_t*,uint32_t*);
typedef rsx_fsr3_status (*vk_fsr3_jitter_fn)(uint32_t,uint32_t,uint64_t,float*,float*,uint32_t*);
typedef rsx_fsr3_status (*vk_fsr3_configure_fn)(rsx_fsr3_vk*,const rsx_fsr3_config*);
typedef rsx_fsr3_status (*vk_fsr3_evaluate_fn)(rsx_fsr3_vk*,VkCommandBuffer,const rsx_fsr3_frame*);
static struct {
    HMODULE module;
    vk_fsr3_requirements_fn requirements;vk_fsr3_create_fn create;vk_fsr3_destroy_fn destroy;
    vk_fsr3_render_size_fn render_size;vk_fsr3_jitter_fn jitter;
    vk_fsr3_configure_fn configure;vk_fsr3_evaluate_fn evaluate;
    rsx_fsr3_vk *context;
    rsx_fsr3_quality quality;
    int camera_logged;
} s_fsr3;

static int vk_fsr3_requested(void){const char *v=getenv("PS3RECOMP_FSR3");return v && !strcmp(v,"1");}
static int vk_sr_requested(void){return vk_dlss_requested() || vk_fsr3_requested();}
static void vk_fsr3_close(void){if(s_fsr3.module)FreeLibrary(s_fsr3.module);memset(&s_fsr3,0,sizeof s_fsr3);}
static int vk_fsr3_open(u32 api)
{
    if(s_fsr3.module)return 1;
    if(!vk_fsr3_requested())return 0;
    if(s_dlss.loader.module){VK_LOG("FSR3 requested while the DLSS bridge is loaded; DLSS is used\n");return 0;}
    if(api<VK_API_VERSION_1_2){VK_LOG("FSR3 requires Vulkan1.2\n");return 0;}
    wchar_t path[32768];
    if(!vk_dlss_wide_env("PS3RECOMP_FSR3_BRIDGE",path,32768) ||
       !(((path[0]|32)>=L'a' && (path[0]|32)<=L'z' && path[1]==L':' && (path[2]==L'\\' || path[2]==L'/')) ||
         (path[0]==L'\\' && path[1]==L'\\' && path[2]!=L'?' && path[2]!=L'.'))){
        VK_LOG("FSR3 requires an absolute PS3RECOMP_FSR3_BRIDGE path\n");return 0;
    }
    /* The bridge's own directory, then System32 (vulkan-1.dll). */
    s_fsr3.module=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!s_fsr3.module){VK_LOG("FSR3 bridge load failed (%lu)\n",(unsigned long)GetLastError());return 0;}
    s_fsr3.requirements=(vk_fsr3_requirements_fn)(void(*)(void))GetProcAddress(s_fsr3.module,"rsx_fsr3_device_requirements");
    s_fsr3.create=(vk_fsr3_create_fn)(void(*)(void))GetProcAddress(s_fsr3.module,"rsx_fsr3_create");
    s_fsr3.destroy=(vk_fsr3_destroy_fn)(void(*)(void))GetProcAddress(s_fsr3.module,"rsx_fsr3_destroy");
    s_fsr3.render_size=(vk_fsr3_render_size_fn)(void(*)(void))GetProcAddress(s_fsr3.module,"rsx_fsr3_render_size");
    s_fsr3.jitter=(vk_fsr3_jitter_fn)(void(*)(void))GetProcAddress(s_fsr3.module,"rsx_fsr3_jitter");
    s_fsr3.configure=(vk_fsr3_configure_fn)(void(*)(void))GetProcAddress(s_fsr3.module,"rsx_fsr3_configure");
    s_fsr3.evaluate=(vk_fsr3_evaluate_fn)(void(*)(void))GetProcAddress(s_fsr3.module,"rsx_fsr3_evaluate");
    if(!s_fsr3.requirements || !s_fsr3.create || !s_fsr3.destroy || !s_fsr3.render_size ||
       !s_fsr3.jitter || !s_fsr3.configure || !s_fsr3.evaluate){
        VK_LOG("FSR3 bridge lacks the rsx_fsr3 export set\n");vk_fsr3_close();return 0;
    }
    VK_LOG("FSR3 explicit bridge loaded\n");return 1;
}
/* Before vkCreateDevice, when DLSS is not prepared: the shaders' features.
 * Instance and device must both be Vulkan 1.2. */
static int vk_fsr3_device_prepare(VkPhysicalDeviceFeatures *want,u32 instance_api)
{
    if(!vk_fsr3_open(instance_api<s_advanced.device_api?instance_api:s_advanced.device_api))return 0;
    rsx_fsr3_requirements r;memset(&r,0,sizeof r);
    if(s_fsr3.requirements(s_vk.phys,&r)!=RSX_FSR3_OK){VK_LOG("FSR3 device requirements unavailable\n");vk_fsr3_close();return 0;}
    if(r.required.shaderStorageImageExtendedFormats)want->shaderStorageImageExtendedFormats=VK_TRUE;
    if(r.required.shaderStorageImageWriteWithoutFormat)want->shaderStorageImageWriteWithoutFormat=VK_TRUE;
    return 1;
}
static void vk_fsr3_device_created(int enabled)
{
    if(!s_fsr3.module)return;
    if(!enabled){vk_fsr3_close();return;}
    rsx_fsr3_device device={s_vk.phys,s_vk.device,pvkGetDeviceProcAddr,0};
    rsx_fsr3_report report;memset(&report,0,sizeof report);
    rsx_fsr3_status status=s_fsr3.create(&device,&s_fsr3.context,&report);
    VK_LOG("FSR3 game-device context status=%d ffx=0x%x fp16=0; evaluatedFrames=0\n",status,(unsigned)report.ffx_result);
    if(status!=RSX_FSR3_OK){s_fsr3.context=NULL;vk_fsr3_close();return;}
    vk_dlss_frame_initialize();
}
/* Caller drains the device first; the shared frame images go afterwards. */
static void vk_fsr3_shutdown(void)
{
    if(s_fsr3.context)s_fsr3.destroy(s_fsr3.context);
    vk_fsr3_close();
}
static rsx_fsr3_quality vk_fsr3_mode(void)
{
    const char *m=getenv("PS3RECOMP_FSR3_MODE");
    if(!m || !*m || !strcmp(m,"quality"))return RSX_FSR3_QUALITY;
    if(!strcmp(m,"balanced"))return RSX_FSR3_BALANCED;
    if(!strcmp(m,"performance"))return RSX_FSR3_PERFORMANCE;
    if(!strcmp(m,"ultra-performance"))return RSX_FSR3_ULTRA_PERFORMANCE;
    if(!strcmp(m,"native"))return RSX_FSR3_NATIVE_AA;
    VK_LOG("PS3RECOMP_FSR3_MODE '%s' unknown; using quality\n",m);
    return RSX_FSR3_QUALITY;
}
static const char *vk_fsr3_mode_name(rsx_fsr3_quality q)
{
    switch(q){case RSX_FSR3_NATIVE_AA:return "native";case RSX_FSR3_BALANCED:return "balanced";
    case RSX_FSR3_PERFORMANCE:return "performance";case RSX_FSR3_ULTRA_PERFORMANCE:return "ultra-performance";
    default:return "quality";}
}
static float vk_fsr3_sharpness(void)
{
    const char *s=getenv("PS3RECOMP_FSR3_SHARPNESS");float v=s && *s?(float)atof(s):0.0f;
    return isfinite(v)?fminf(fmaxf(v,0.0f),1.0f):0.0f;
}
static int vk_fsr3_reactive(void){const char *v=getenv("PS3RECOMP_FSR3_REACTIVE");return v && !strcmp(v,"1");}
/* The frame shader reconstructs view depth as P14/(z-P10) from a [0,1] depth
 * with the sky at 1, i.e. z = P10 + P14/depth: near at z=0, far at z=1. */
static void vk_fsr3_camera(const float projection[16],float *near_plane,float *far_plane,float *fov)
{
    const float a=projection[10],b=projection[14],y=fabsf(projection[5]);
    float n=a!=0?-b/a:0,f=a!=1?b/(1-a):1e6f;
    if(!isfinite(f) || f>1e6f)f=1e6f;
    float v=y>0?2*atanf(1/y):0;
    if(!isfinite(n) || n<=0 || !(f>n) || !isfinite(v) || v<=0 || v>=3.1415f){
        if(!s_fsr3.camera_logged){s_fsr3.camera_logged=1;
            VK_LOG("FSR3 camera from projection unusable (P10=%g P14=%g P5=%g); using near 0.1 far 1000 fov 1.0\n",a,b,projection[5]);}
        n=.1f;f=1000;v=1;
    }
    *near_plane=n;*far_plane=f;*fov=v;
}
static rsx_fsr3_image vk_fsr3_image(vk_image *im,VkFormat format,u32 w,u32 h,VkImageLayout layout)
{
    const VkImageUsageFlags use=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return (rsx_fsr3_image){im->img,format,w,h,use,layout};
}
#else
static int vk_fsr3_requested(void){return 0;}
static int vk_sr_requested(void){return vk_dlss_requested();}
static int vk_fsr3_device_prepare(VkPhysicalDeviceFeatures *want,u32 instance_api){(void)want;(void)instance_api;return 0;}
static void vk_fsr3_device_created(int enabled){(void)enabled;}
static void vk_fsr3_shutdown(void){}
#endif

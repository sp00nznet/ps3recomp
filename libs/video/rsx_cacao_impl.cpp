// Keep upstream source unchanged and Vulkan loader availability optional.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#define RSX_CACAO_MAP_VULKAN
#include "rsx_cacao_dispatch.h"
#ifndef FFX_CACAO_ENABLE_VULKAN
#define FFX_CACAO_ENABLE_VULKAN
#endif
#include "../../third_party/cacao/src/ffx_cacao_impl.cpp"

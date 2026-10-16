# FidelityFX CACAO

The **FidelityFX CACAO** library implements screen space ambient occlusion for use in real time applications. A full sample can be found on the [FidelityFX CACAO Github page](https://github.com/GPUOpen-Effects/FidelityFX-CACAO).

# Project Integration

FidelityFX CACAO comes with two main header files, `ffx-cacao/inc/ffx_cacao.h` and `ffx-cacao/inc/ffx_cacao_impl.h`. The file `ffx-cacao/inc/ffx_cacao.h` contains reusable C++ functions and struct definitions for integration of FidelityFX CACAO into custom engines. The functions declared in this header file are defined in `ffx-cacao/src/ffx_cacao.cpp`. The header file `ffx-cacao/inc/ffx_cacao_impl.h` is for use in quick integration of FidelityFX CACAO into DX12 and Vulkan engines. The functions declared in this file are defined in `ffx-cacao/src/ffx_cacao_impl.cpp`, which serves as a reference implementation of FidelityFX CACAO.

# Reusable Functions and Structs

The reusable functions and structs provided in `ffx-cacao/src/ffx_cacao.h` are documented via doxygen comments in the header file itself. The functions and structs are used to initialise the constant buffers used by FidelityFX CACAO from a user friendly settings struct `FFX_CACAO_Settings`.

# Reference Implementation

The reference implementation of FidelityFX CACAO supports three compile time options. These are:

```C++
FFX_CACAO_ENABLE_D3D12
FFX_CACAO_ENABLE_VK
FFX_CACAO_ENABLE_PROFILING
```

For use with D3D12 or Vulkan, the symbols `FFX_CACAO_ENABLE_D3D12` or `FFX_CACAO_ENABLE_VK` must be defined. If you wish to get detailed timings from FFX CACAO the symbol `FFX_CACAO_ENABLE_PROFILING` must be defined. These symbols can either be defined in the header `ffx-cacao/inc/ffx_cacao_impl.h` itself by uncommenting the respective definitions, or they can defined in compiler flags. The provided sample of FFX CACAO defines these symbols using compiler flags.

# Context Initialisation and Shutdown

First the FFX CACAO header must be included. This can be found at `ffx-cacao/inc/ffx_cacao_impl.h`. Then a context must be created. This is usually done only once per device. To create a context you must first query for the size of a context, allocate space for a context, then inintialise the context.


For D3D12 the initialisation and shutdown processes are as follows:

```C++
// initialisation
size_t ffxCacaoContextSize = ffxCacaoD3D12GetContextSize();
FfxCacaoD3D12Context *context = (FfxCacaoD3D12Context*)malloc(ffxCacaoContextSize);
assert(context);
FfxCacaoStatus status = ffxCacaoD3D12InitContext(context, d3d12Device);
assert(status == FFX_CACAO_STATUS_OK);
...
// finalisation
status = ffxCacaoD3D12DestroyContext(context);
assert(status == FFX_CACAO_STATUS_OK);
free(context);
```

The only argument required for initialisation of a D3D12 context is an `ID3D12Device*` for the D3D12 device.

For Vulkan the initialisation and shutdown processes are as follows:

```C++
// initialisation
size_t ffxCacaoContextSize = ffxCacaoVkGetContextSize();
FfxCacaoVkContext *context = (FfxCacaoVkContext*)malloc(ffxCacaoContextSize);
assert(context);
FfxCacaoVkCreateInfo info = {};
info.physicalDevice = vkPhysicalDevice;
info.device = vkDevice;
info.flags = FFX_CACAO_VK_CREATE_USE_16_BIT | FFX_CACAO_VK_CREATE_USE_DEBUG_MARKERS | FFX_CACAO_VK_CREATE_NAME_OBJECTS;
FfxCacaoStatus status = ffxCacaoVkInitContext(context, &info);
assert(status == FFX_CACAO_STATUS_OK);
...
// finalisation
status = ffxCacaoVkDestroyContext(context);
assert(status == FFX_CACAO_STATUS_OK);
free(context);
```

To initialise the FFX CACAO context in Vulkan, the parameters of the `FfxCacaoVkCreateInfo` struct must be filled in. These are the Vulkan physical device and Vulkan device, and a field of flags. The flags is a bitwise combination of the following options. The option `FFX_CACAO_VK_CREATE_USE_16_BIT` enables 16 bit optimisations, and requires a Vulkan device created using 16 bit extensions. This option is strongly recommended for compatible devices. The options `FFX_CACAO_VK_CREATE_USE_DEBUG_MARKERS` and `FFX_CACAO_VK_CREATE_NAME_OBJECTS` will add debug markers and name objects (e.g. textures, shaders) to aid inspection of FFX CACAO with a frame debugger.

# Screen Size Dependent Resource Initialisation

Once the context is initialised, it will need to have screen size dependent resources initialised each time the screen size is changed. To do this, an `FfxCacaoD3D12ScreenSizeInfo` struct must be filled out. The FFX CACAO effect is computed using a depth buffer and optional normal buffer. FFX CACAO writes its output to a user provided output buffer. The depth buffer, normal buffer and output buffer provided to FFX CACAO must all be the same size.


For FFX CACAO D3D12, the process is as follows:

```C++
// initialisation
FfxCacaoD3D12ScreenSizeInfo screenSizeInfo = {};
screenSizeInfo.width = /* width of the input/output buffers */;
screenSizeInfo.height = /* height of the input/output buffers */;
screenSizeInfo.depthBufferResource = /* ID3D12Resource* for the depth input buffer */;
screenSizeInfo.depthBufferSrvDesc = /* D3D12_SHADER_RESOURCE_VIEW_DESC for the depth input buffer */;
screenSizeInfo.normalBufferResource = /* ID3D12Resource* for the normal input buffer - or NULL if none shall be provided */;
screenSizeInfo.normalBufferSrvDesc = /* D3D12_SHADER_RESOURCE_VIEW_DESC for the normal input buffer */;
screenSizeInfo.outputResource = /* ID3D12Resource* for the output buffer */;
screenSizeInfo.depthBufferSrvDesc = /* D3D12_SHADER_RESOURCE_VIEW_DESC for the depth output */;
status = ffxCacaoD3D12InitScreenSizeDependentResources(context, &screenSizeInfo);
assert(status == FFX_CACAO_STATUS_OK);
...
// finalisation
status = ffxCacaoD3D12DestroyScreenSizeDependentResources(context);
assert(status == FFX_CACAO_STATUS_OK);
```

For FFX CACAO Vulkan, the process is as follows:

```C++
// Initialisation
FfxCacaoVkScreenSizeInfo screenSizeInfo = {};
screenSizeInfo.width = /* width of the input/output buffers */;
screenSizeInfo.height = /* height of the input/output buffers */;
screenSizeInfo.depthView = /* a VkImageView for the depth buffer, should be in layout VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL */;
screenSizeInfo.normalsView = /* an optional VkImageView for the normal buffer (VK_NULL_HANDLE if not provided), should be in layout VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL */;
screenSizeInfo.output = /* a VkImage for writing the output of FFX CACAO */;
screenSizeInfo.outputView = /* a VkImageView corresponding to the VkImage for writing the output of FFX CACAO */;
status = ffxCacaoVkInitScreenSizeDependentResources(context, &screenSizeInfo);
assert(status == FFX_CACAO_STATUS_OK);
...
// finalisation
status = ffxCacaoVkDestroyScreenSizeDependentResources(context);
assert(status == FFX_CACAO_STATUS_OK);
```

# Initialising/Updating FFX CACAO Settings

The settings for the FFX CACAO effect may be changed via the `FfxCacaoSettings` struct and the `ffxCacaoD3D12UpdateSettings` or `ffxCacaoVkUpdateSettings` functions as follows.

```C++
FfxCacaoSettings settings = {};
settings.radius = /* world view radius of the occlusion sphere */;
settings.shadowMultiplier = /* effect strength linear multiplier */;
settings.shadowPower = /* effect strength power multiplier */;
settings.shadowClamp = /* effect max limit */;
settings.horizonAngleThreshold = /* minimum horizon angle for contributions to occlusion to limit self shadowing */
settings.fadeOutFrom = /* effect fade out from world space distance */;
settings.fadeOutTo = /* effect fade out to world space distance */;
settings.qualityLevel = /* the quality of the effect, ranging from lowest to highest (adaptive). This affects the number of samples taken to generate SSAO. */;
settings.adaptiveQualityLimit = /* quality limit for adaptive quality */;
settings.blurPassCount = /* a number of edge sensitive blurs from 1 to 8 to perform after SSAO generation */;
settings.sharpness = /* how much to bleed over edges - 0 = ignore edges, 1 = don't bleed over edges */;
settings.temporalSupersamplingAngleOffset = /* sampling angle offset for temporal super sampling */;
settings.temporalSupersamplingRaidusOffset = /* sampling effect radius offset for temporal super sampling */;
settings.detailShadowStrength = /* used to generate details in high res AO */;
settings.generateNormals = /* should the effect generate normals from the depth buffer or use a provided normal buffer */;
settings.bilateralSigmaSquared = /* a parameter for use in bilateral upsampling. Higher values create more blur to help reduce noise */;
settings.bilateralSimilarityDistanceSigma = /* a parameter for use in bilateral upsampling. Lower values create reduce bluring across edge boundaries */;
```

These settings can be set to sensible defaults from the constant `FFX_CACAO_DEFAULT_SETTINGS` and updated using the function `ffxCacaoD3D12UpdateSettings` or `ffxCacaoVkUpdateSettings` as follows.

```C++
FfxCacaoSettings settings = FFX_CACAO_DEFAULT_SETTINGS;
status = ffxCacaoD3D12UpdateSettings(context, &settings);
assert(status == FFX_CACAO_STATUS_OK);
```

Note that the `FFX_CACAO_DEFAULT_SETTINGS` provides a sensible quick start for high quality settings. The parameters `radius`, `fadeOutFrom` and `fadeOutTo` should
be changed to match the world space of the target scene, and the parameter `blurPassCount` is recommended to be increased for lower quality settings. A more complete
set of sensible defaults may be found in the FFX CACAO sample in the file `sample/src/Common/FFX_CACAO_Common.h`, where multiple parameters have been varied to move
from high to low quality presets.

# Drawing

In D3D12, FFX CACAO can be called to add commands to a `ID3D12GraphicsCommandList` using the `ffxCacaoD3D12Draw` function as follows:

```C++
FfxCacaoMatrix4x4 proj = /* row major projection matrix */;
FfxCacaoMatrix4x4 normalsToView = /* row major matrix to convert normals to viewspace */
status = ffxCacaoD3D12Draw(context, commandList, &proj, &normalsToView);
assert(status == FFX_CACAO_STATUS_OK);
```

In Vulkan, FFX CACAO can add commands to a `VkCommandBuffer` using the `ffxCacaoVkDraw` function as follows:

```C++
FfxCacaoMatrix4x4 proj = /* row major projection matrix */;
FfxCacaoMatrix4x4 normalsToView = /* row major matrix to convert normals to viewspace */
status = ffxCacaoVkDraw(context, commandBuffer, &proj, &normalsToView);
assert(status == FFX_CACAO_STATUS_OK);
```

The matrix `proj` is the projection matrix used from viewspace to normalised device coordinates. The matrix `normalsToView` is a matrix to convert the normals provided in the normal buffer to viewspace.

# Profiling

Finally, if the preprocessor symbol `FFX_CACAO_ENABLE_PROFILING` is defined, then detailed timings can be read from FFX CACAO using the functions `ffxCacaoD3D12GetDetailedTimings` and `ffxCacaoVkGetDetailedTimings` for D3D12 and Vulkan respectively. These functions should be called as follows:

```C++
FfxCacaoDetailedTiming timings = {};
uint64_t gpuTicksPerMicrosecond;
FfxCacaoStatus status = ffxCacaoD3D12GetDetailedTimings(context, &timings, &gpuTicksPerMicrosecond);
assert(status == FFX_CACAO_STATUS_OK);
```

The timings returned are in GPU ticks. These can be converted into seconds using the value returned in the `gpuTicksPerMicrosecond` parameter above.

Or in Vulkan:

```C++
FfxCacaoDetailedTiming timings = {};
FfxCacaoStatus status = ffxCacaoD3D12GetDetailedTimings(context, &timings);
assert(status == FFX_CACAO_STATUS_OK);
```

The timings returned are measured in GPU ticks, and will need to be converted using th GPU ticks per microsecond parameter available from `vkGetPhysicalDeviceLimits`.

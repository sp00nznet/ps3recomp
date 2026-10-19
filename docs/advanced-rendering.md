# Advanced rendering architecture and current contract

## 2026-10-19 integration checkpoint

This isolated branch imports the preserved 2026-10-18 rendering prototype onto
SDK base `96080cf63eb298c57aa682a47fd95935104911bc`. The merge retains the current
HLSL disk cache, merged render passes, deferred uploads, and GPU depth-to-ARGB
conversion. The shader-cache regression suite passes. GLSL ray kernels use a
separate uncached Vulkan 1.2/SPIR-V 1.5 compiler entry point; the existing HLSL
cache key and binding conventions remain unchanged.

RT AO remains an explicit diagnostic (`PS3RECOMP_RSX_AO=rayquery`). The backend
enables BDA, acceleration structures and ray queries only when requested and
supported. Current-frame immutable scene epochs feed actual vertex-program
replay, BLAS/TLAS construction, alpha-qualified secondary rays and a world-color
composite. This is not a material/light scene reconstruction or path tracer.

The initial integrated source compiled, with CPU epoch, alpha, BVH, capability
and shader-cache suites passing. Follow-up changes allow opaque draws with no
fragment constants, retain the required constants for alpha contracts, and bind
a valid nonzero Vulkan descriptor for the empty opaque case. Those follow-up
changes and the corrected single-sample/MSAA2 live fixture await GPU validation.
Game and compilation tests were stopped for a concurrent user-controlled game
session. No accepted player binary or launcher setting was replaced.

DLSS SR/FG are not integrated. Stable object identity, current/previous
transformed positions, jitter, matching depth and presentation integration are
still missing. PT, RT lighting, reflections and shadows need qualified material
and light semantics beyond the existing alpha contracts. Unlocked simulation
is outside this renderer branch; current timing/cloth equivalence is unverified.

The earlier architecture notes below describe the prototype's original
development stages. In particular, optional logical features ARE now enabled
for the diagnostic ray-query route, but that does not establish a qualified
gameplay mode.

This branch contains capability inspection and an experimental scene bridge. It does not yet expose RT, PT or DLSS as gameplay settings. The accepted raster renderer remains the fallback, preserving the selected FSR1, FXAA and CACAO settings. FSR1 and CACAO must never be labeled DLSS or ray tracing.

## Implemented interfaces

- `rsx_vulkan_backend_advanced_capabilities()` returns physical-device observations separately from renderer availability. Read on the owning render thread. Values -1/0/1 mean unknown/absent/supported. Successful backend initialization is distinct from a successful optional feature query. No advanced logical-device features are enabled by this probe.
- `rsx_vulkan_backend_advanced_report(buffer, capacity)` returns schema-1 JSON. NULL/0 measures the payload, excluding NUL. A short destination returns -1 and is emptied. Before initialization and after shutdown device observations are unknown. All gameplay RT/PT/DLSS availability and active flags remain false.
- `rsx_scene_compute_source()` wraps the generated guest vertex program in compute without substituting attribute0 for shader output. Compact attribute layout and exact 514-float4 constant block are required. Vertex texture programs are rejected until texture/sampler resource capture exists.
- `rsx_vulkan_backend_replay_positions()` explicitly executes that wrapper on the owning render thread, synchronously. It returns exact post-shader clip coordinates before Vulkan Y inversion, including the guest viewport epilogue. Nothing automatically invokes it per frame. Caller output is unchanged on unsupported input or failure.
- `rsx_scene_clip_to_view()` accepts a qualified, matching final projection in the existing AO matrix convention. It is not an arbitrary inverse-camera heuristic. Camera-plane W=0 and nonfinite/singular inputs reject.
- `test_vulkan_ray_query_geometry --ao` builds actual Vulkan BLAS/TLAS and traces deterministic secondary rays against captured opaque triangles. Its standalone output is an offline AO diagnostic, never a gameplay setting. `scene_ray_reference --ao` supplies an independent CPU BVH comparison. Camera-facing geometric normals, double-sided intersections, constant bias/distance and missing alpha/material handling are explicit limits.
- `rsx_scene_bvh` is an immutable CPU triangle-ray reference with copied input, original triangle identities, nearest hits and double-sided geometric normals. No material or scene completeness is inferred.

## Staged integration boundary

1. Collect scene draws by depth target, clear epoch and frame, with original draw identity, primitive/index order, culling and alpha state. Preserve exact vertex attributes, constants, textures and shader provenance. Do not mix shadow cameras, fullscreen passes or HUD with the world pass.
2. Replay the actual vertex program. Reconstruct a common view or world coordinate system only when all participating draws agree on camera/projection provenance. Keep object history separate from camera history, and reject ambiguous identity matches.
3. Build/update geometry acceleration structures from validated triangles. The standalone ray-query validator tests real BLAS/TLAS traversal against the independent CPU reference. It is not a game rendering mode. Alpha-tested foliage needs original UV/texture/alpha evaluation; treating those cards as opaque is incorrect.
4. An eventual RT AO/shadow pass may use qualified geometry and original color, composed before the HUD. Unknown or stale scene inputs must retain the original color and explain the rejection. Screen-space reconstructed depth is not a substitute for a complete scene.
5. Path tracing additionally needs explicitly identified material reflectance, normals, emissive/light parameters and an integrator. Arbitrary raster shader outputs, baked light and bloom do not uniquely identify those quantities. Approximation choices must be named; no native path-tracing claim follows from ray traversal.
6. DLSS SR needs a complete per-frame resource contract: pre-HUD input color, matching depth, camera/object motion vectors, render/output extents, jitter convention and reset handling, exposure convention, and actual SDK evaluation/output delivery. Camera-only or fabricated zero motion must not be accepted as complete moving-object history. Ray Reconstruction and Frame Generation are separate features, not implied by SR.

## Future launcher contract

Read availability, active state and reason separately. Do not enable a mode from GPU name, extension presence, or a compiled test. Current RT/PT/DLSS options must be disabled/unavailable with the report reason. Existing raster settings remain available. This branch does not redesign the launcher.

Display owns render/output sizing and aspect behavior. Its content rectangle can exclude host-added bars before fitting the window; a future temporal/postprocess stage must preserve that rectangle and signal history invalidation on size/aspect/camera changes. Performance owns guest timing. This branch never changes the guest timebase or frame limiter.

## Dependencies and primary references

No NVIDIA SDK, redistributable, private model weights, credentials or keys are added. The new geometry reference uses existing Vulkan headers/loader and glslang. Existing project licensing/notices remain in force. A future DLSS redistributable integration needs a specific license-compatible distribution design; local compilation is not distribution approval.

- [NVIDIA DLSS resource and constant contract](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md)
- [Official DLSS SDK](https://github.com/NVIDIA/DLSS)
- [NVIDIA RTX SDK license](https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt)
- [Khronos ray tracing guide](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html)
- [Khronos acceleration structures](https://docs.vulkan.org/spec/latest/chapters/accelstructures.html)

Visual correctness, temporal stability, matched-scene performance and coverage remain separate acceptance stages. CPU or GPU synthetic tests do not establish gameplay support.

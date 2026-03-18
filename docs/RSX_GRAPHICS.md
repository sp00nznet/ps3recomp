# RSX Graphics Translation

How ps3recomp translates PS3 RSX GPU commands to host graphics APIs.

---

## Architecture Overview

```
Game code (recompiled C)
    │
    ▼
cellGcmSys HLE                    ← libs/video/cellGcmSys.c
  │ State management: display buffers, IO mapping, flip/VBlank,
  │ tile/zcull config, command buffer control (put/get/ref)
  │
  ▼
RSX Command Processor             ← libs/video/rsx_commands.c
  │ Parses NV47xx FIFO command buffer
  │ Tracks GPU state: surfaces, viewport, blend, depth,
  │ textures, vertex attribs, shaders, draw calls
  │ Dispatches to registered backend via callbacks
  │
  ├──▶ Null Backend               ← libs/video/rsx_null_backend.c
  │    Win32 window + GDI clear color (debugging)
  │
  ├──▶ D3D12 Backend              ← libs/video/rsx_d3d12_backend.c
  │    Real GPU rendering (Windows)
  │
  └──▶ Vulkan Backend             ← (planned)
       Cross-platform GPU rendering
```

## The Three Layers

### Layer 1: cellGcmSys (HLE Module)

The game interacts with the GPU through `cellGcmSys` — the PS3 SDK's RSX interface. Our HLE implementation handles:

- **Initialization**: `cellGcmInit()` sets up the command buffer, local memory, and offset tables
- **Display buffers**: `cellGcmSetDisplayBuffer()` registers framebuffer regions
- **Flip control**: `cellGcmSetFlipMode()`, `cellGcmResetFlipStatus()`, VBlank handlers
- **Memory mapping**: `cellGcmMapMainMemory()`, `cellGcmAddressToOffset()` for GPU-visible memory
- **Tile/Zcull**: Surface tiling and depth culling configuration

The game writes NV47xx GPU method commands to a command buffer. On real hardware, the RSX reads this buffer and executes GPU commands. In ps3recomp, we intercept the buffer.

### Layer 2: RSX Command Processor

The command processor (`rsx_commands.c`) parses the NV47xx FIFO protocol:

**Command buffer format:**
```
Header: [31:29] type | [28:18] count | [12:2] method
Data:   count × 32-bit words
```

- **Type 0** (increasing): method, method+4, method+8... for each data word
- **Type 2** (non-increasing): same method repeated for each data word

**State tracking** — the processor maintains `rsx_state` with:

| Category | Tracked State |
|----------|--------------|
| Surfaces | Color/depth offsets, pitches, format, clip rect |
| Viewport | Position, size, depth range |
| Blend | Enable, src/dst factors, equation, color |
| Depth | Test enable, function, write mask |
| Stencil | Test, function, ref, mask, fail/zfail/zpass ops |
| Cull | Enable, face, front face winding |
| Textures | 16 units × offset, format, address, filter, rect |
| Vertices | 16 attribs × format, offset, stride, enabled |
| Shaders | Fragment program addr, vertex program load slot |
| Draw | Primitive type, draw arrays, draw indexed |

**Dirty flags** ensure the backend only receives state changes, not redundant updates.

### Layer 3: Graphics Backend

The `rsx_backend` struct defines callback functions that the command processor calls:

```c
typedef struct rsx_backend {
    void* userdata;
    int  (*init)(void*, u32 width, u32 height);
    void (*shutdown)(void*);
    void (*begin_frame)(void*);
    void (*end_frame)(void*);
    void (*present)(void*, u32 buffer_id);
    void (*set_render_target)(void*, const rsx_state*);
    void (*set_viewport)(void*, const rsx_state*);
    void (*set_blend)(void*, const rsx_state*);
    void (*set_depth_stencil)(void*, const rsx_state*);
    void (*set_color_mask)(void*, const rsx_state*);
    void (*set_alpha_test)(void*, const rsx_state*);
    void (*set_shader)(void*, const rsx_state*);
    void (*set_vertex_attribs)(void*, const rsx_state*);
    void (*clear)(void*, u32 flags, u32 color, float depth, u8 stencil);
    void (*draw_arrays)(void*, u32 primitive, u32 first, u32 count);
    void (*draw_indexed)(void*, u32 primitive, u32 offset, u32 count);
    void (*bind_texture)(void*, u32 unit, const rsx_texture_state*);
} rsx_backend;
```

Dirty state is flushed to the backend before each draw call (in `SET_BEGIN_END`).

---

## D3D12 Backend

The D3D12 backend (`rsx_d3d12_backend.c`) translates RSX state to Direct3D 12:

### Current Implementation (Phase 1)
- Device creation with feature level 11.0
- Double-buffered swap chain (flip-discard)
- Clear render target to RSX clear color
- VSync present
- Fence-based frame synchronization

### Planned (Phase 2)
- Vertex buffer upload from RSX vertex attribute state
- Root signature and basic pipeline state
- RSX primitive types → D3D12 topology mapping
- Basic vertex-colored rendering

### Planned (Phase 3)
- RSX fragment/vertex program → HLSL shader translation
- Texture upload and sampling
- Depth/stencil buffer creation
- Framebuffer management (multiple render targets)
- Tile/Zcull → D3D12 depth optimization hints

---

## RSX Primitive Types

| RSX | Value | D3D12 Topology |
|-----|-------|---------------|
| Points | 1 | D3D_PRIMITIVE_TOPOLOGY_POINTLIST |
| Lines | 2 | D3D_PRIMITIVE_TOPOLOGY_LINELIST |
| Line Loop | 3 | (emulated with line strip + extra edge) |
| Line Strip | 4 | D3D_PRIMITIVE_TOPOLOGY_LINESTRIP |
| Triangles | 5 | D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST |
| Triangle Strip | 6 | D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP |
| Triangle Fan | 7 | (emulated with triangle list) |
| Quads | 8 | (emulated with triangle list, 2 tris per quad) |
| Quad Strip | 9 | (emulated) |

Note: D3D12 doesn't support triangle fans, quads, or line loops natively. These need to be converted to triangle lists or line strips with index buffer manipulation.

---

## Shader Translation (Future)

The RSX uses a proprietary shader ISA based on NV40/NV47:

- **Vertex programs**: Stored in transform program memory, loaded via `SET_TRANSFORM_PROGRAM_LOAD`. Up to 512 instructions.
- **Fragment programs**: Stored in VRAM/main memory at the address in `SET_SHADER_PROGRAM`. Variable length.

Translation approach:
1. Parse RSX shader bytecode (NV40 ISA)
2. Convert to an intermediate representation
3. Emit HLSL (D3D12) or GLSL/SPIR-V (Vulkan)
4. Compile at runtime with D3DCompile / glslang
5. Cache compiled shaders by hash

Reference: RPCS3's `RSXVertexProgram` / `RSXFragmentProgram` decompilers in `rpcs3/Emu/RSX/Program/`.

---

## For Game Porters

When porting a game:

1. **Start with the null backend** — verify that cellGcmSys calls succeed and the game loop runs
2. **Switch to D3D12** — see the clear color change as the game initializes
3. **Log draw calls** — the backend logs draw_arrays/draw_indexed to understand the rendering pattern
4. **Identify shaders** — check which vertex/fragment programs the game loads
5. **Implement shader translation** — start with the game's specific shaders, not a general-purpose translator

Most PS3 games use only 10-50 unique shader programs. A per-game shader cache is often more practical than a full ISA translator for the first port.

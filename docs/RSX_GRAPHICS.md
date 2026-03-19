# RSX Graphics Translation

How ps3recomp translates PS3 RSX GPU commands to host graphics APIs.

---

## Architecture Overview

The RSX graphics pipeline has three layers, each in its own source file:

```
Game code (recompiled C)
    │
    ▼
cellGcmSys HLE                    ← libs/video/cellGcmSys.c (33+ functions)
  │ State management: display buffers, IO mapping, flip/VBlank,
  │ tile/zcull config, command buffer control (put/get/ref),
  │ local memory allocator, offset tables, timestamps
  │
  ▼
RSX Command Processor             ← libs/video/rsx_commands.c
  │ Parses NV47xx FIFO command buffer
  │ Tracks all GPU state with dirty flags
  │ Dispatches to registered backend via callbacks
  │
  ├──▶ Null Backend               ← libs/video/rsx_null_backend.c
  │    Win32 window + GDI clear color (debugging)
  │
  ├──▶ D3D12 Backend              ← libs/video/rsx_d3d12_backend.c
  │    Real GPU rendering (Windows) with compiled shaders
  │
  └──▶ Vulkan Backend             ← (planned)
       Cross-platform GPU rendering

Supporting files:
  libs/video/rsx_primitives.h      ← RSX → D3D12 topology mapping + index conversion
  libs/video/rsx_vertex_formats.h  ← RSX → DXGI format mapping
  libs/video/rsx_d3d12_shaders.h   ← Built-in HLSL shaders
  libs/video/cellResc.c            ← Resolution scaling (16 functions)
  libs/video/cellVideoOut.c        ← Video output configuration
```

---

## Layer 1: cellGcmSys (HLE Module)

**File:** `libs/video/cellGcmSys.c` (~800 lines, 33+ functions)

The game interacts with the GPU through `cellGcmSys` — the PS3 SDK's RSX interface. Our HLE implementation provides:

### Initialization
- `cellGcmInit(cmdSize, ioSize, ioAddress)` — Sets up the RSX configuration struct (local memory address/size, IO region, clock frequencies), initializes the offset translation tables, and creates the command buffer control registers.
- `cellGcmGetConfiguration(config)` — Returns the `CellGcmConfig` struct with local/IO memory addresses and sizes. Games read this to know where GPU memory starts.
- `cellGcmGetControlRegister()` — Returns a pointer to the `CellGcmControl` struct (`put`, `get`, `ref` registers). The game writes the `put` pointer to advance the command buffer. **Important:** This must return a guest-accessible address (allocated in guest VM), not a host pointer.

### Display Buffers
- `cellGcmSetDisplayBuffer(id, offset, pitch, width, height)` — Registers a framebuffer region. Games typically register 2 buffers for double-buffering.
- `cellGcmSetFlipMode(mode)` — VSYNC or HSYNC.
- `cellGcmSetFlipHandler(handler)` / `cellGcmSetVBlankHandler(handler)` — Callback registration. In recomp, these store guest function pointers but don't fire automatically (no real VBlank interrupt).
- `cellGcmResetFlipStatus()` / `cellGcmGetFlipStatus()` — Flip state tracking.
- `cellGcmGetLastFlipTime()` — Returns host timestamp via `QueryPerformanceCounter` (Windows) or `clock_gettime` (POSIX).

### Memory Management
- `cellGcmMapMainMemory(ea, size, offset)` — Maps main memory for GPU access. Uses a bump allocator with 1MB alignment and tracks mappings in an offset table.
- `cellGcmMapEaIoAddress(ea, io, size)` — Explicit EA→IO mapping with overlap detection.
- `cellGcmAddressToOffset(address, offset)` — Translates effective address to RSX-relative offset using the populated offset tables.
- `cellGcmGetOffsetTable(table)` — Returns pointers to the ioAddress/eaAddress translation arrays.

### Tile and Zcull
- `cellGcmSetTile(index, ...)` — Configures one of 15 tile regions with compression parameters.
- `cellGcmSetZcull(index, ...)` — Configures one of 8 zcull (hierarchical depth) regions.
- `cellGcmBindTile(index)` / `cellGcmUnbindTile(index)` — Activate/deactivate tile regions.
- `cellGcmSetTileInfo(...)` — Alternative tile configuration (same as SetTile, needed by Tokyo Jungle).

### Labels and Reports
- `cellGcmGetLabelAddress(index)` — Returns a guest pointer to one of 256 label slots (4 bytes each). **Must be in guest VM memory** for the game to read/write via vm_read/vm_write.
- `cellGcmGetReportDataAddress(index)` — Returns pointer to report data (16 bytes: timestamp + value).
- `cellGcmGetNotifyDataAddress(index)` — Notify data area (256 slots, used by Tokyo Jungle).

### Reference
- Based on RPCS3's `rpcs3/Emu/RSX/rsx_methods.cpp` and `cellGcmSys.cpp`
- PS3 SDK documentation: `cell/target/ppu/include/cell/gcm/` headers

---

## Layer 2: RSX Command Processor

**File:** `libs/video/rsx_commands.c` (~400 lines) + `rsx_commands.h` (~300 lines)

### How It Works

On the real PS3, the game writes NV47xx GPU method commands to a FIFO ring buffer. The RSX hardware reads these commands and executes them. In ps3recomp, we intercept the command buffer after the game writes to it and process the methods ourselves.

### Command Buffer Format (NV47xx FIFO)

Each command is a 32-bit header followed by N data words:

```
Header: [31:29] type | [28:18] count | [17:13] subchannel | [12:2] method | [1:0] flags
```

| Type | Name | Behavior |
|------|------|----------|
| 0 | Increasing | Writes data to method, method+4, method+8... |
| 2 | Non-increasing | Writes all data to the same method address |
| 1 | Jump | Changes command buffer read position |
| 3 | Call/Return | Subroutine call/return |

### State Tracking

The `rsx_state` struct tracks ALL GPU state that methods modify:

| Category | Fields | Method Range |
|----------|--------|--------------|
| **Surfaces** | color/depth offsets, pitches, format, clip rect, color target | 0x200-0x23C |
| **Viewport** | x, y, width, height, depth range (clip_min/max) | 0x300-0x398 |
| **Scissor** | x, y, width, height | 0x8C0-0x8C4 |
| **Clear** | color value (ARGB), zstencil value | 0x1D0, 0x1D8, 0x1D94 |
| **Blend** | enable, src/dst factors, equation, color | 0x310-0x350 |
| **Depth** | test enable, function, write mask | 0x304-0x30C |
| **Stencil** | enable, func, ref, mask, fail/zfail/zpass ops | 0x360-0x378 |
| **Cull** | enable, face (front/back), winding (CW/CCW) | 0x2BC-0x2C4 |
| **Color mask** | ARGB channel write enables | 0x028 |
| **Alpha test** | enable, function, reference value | 0x104-0x10C |
| **Textures** | 16 units × 8 registers (offset, format, address, control, filter, rect, border) | 0x1A00-0x1BFF |
| **Vertices** | 16 attribs × format + offset (type, size, stride, enabled) | 0x1680-0x177F |
| **Shaders** | fragment program addr, vertex load slot, constants, output mask | 0x8E4, 0x1E9C, 0x1EFC |
| **Draw** | primitive type, draw_arrays (first+count), draw_indexed | 0x1808, 0x1814, 0x1820 |

Each category has a **dirty flag**. When a method modifies state, the dirty flag is set. Before a draw call (`SET_BEGIN_END`), all dirty state is flushed to the backend via callbacks. This ensures the backend only processes actual changes, not redundant updates.

### Processing Flow

```c
// Game writes methods to command buffer via put pointer
// We process them when the game calls flip or when we detect put > get

int rsx_process_command_buffer(rsx_state* state, const u32* buf, u32 size);
// Parses headers, extracts method+data, calls rsx_process_method for each

int rsx_process_method(rsx_state* state, u32 method, u32 data);
// Updates rsx_state and sets dirty flags
// For draw commands, dispatches to backend
```

### Reference
- NV47xx method register documentation: [envytools rnndb](https://envytools.readthedocs.io/)
- RPCS3's method handling: `rpcs3/Emu/RSX/rsx_methods.cpp`

---

## Layer 3: Graphics Backends

### Backend Interface

**File:** `rsx_commands.h` (the `rsx_backend` struct)

Every backend implements these callbacks:

```c
typedef struct rsx_backend {
    void* userdata;              // backend-specific state pointer

    // Lifecycle
    int  (*init)(void*, u32 w, u32 h);
    void (*shutdown)(void*);
    void (*begin_frame)(void*);
    void (*end_frame)(void*);
    void (*present)(void*, u32 buffer_id);

    // State changes (called when dirty flags set, before draw)
    void (*set_render_target)(void*, const rsx_state*);
    void (*set_viewport)(void*, const rsx_state*);
    void (*set_blend)(void*, const rsx_state*);
    void (*set_depth_stencil)(void*, const rsx_state*);
    void (*set_color_mask)(void*, const rsx_state*);
    void (*set_alpha_test)(void*, const rsx_state*);
    void (*set_shader)(void*, const rsx_state*);
    void (*set_vertex_attribs)(void*, const rsx_state*);

    // Rendering
    void (*clear)(void*, u32 flags, u32 color, float depth, u8 stencil);
    void (*draw_arrays)(void*, u32 primitive, u32 first, u32 count);
    void (*draw_indexed)(void*, u32 primitive, u32 offset, u32 count);
    void (*bind_texture)(void*, u32 unit, const rsx_texture_state*);
} rsx_backend;
```

Register a backend with `rsx_set_backend(&my_backend)`. Only one backend can be active at a time.

### Null Backend

**File:** `libs/video/rsx_null_backend.c`

Debug backend that creates a Win32 window and displays the RSX clear color via GDI. Shows an FPS counter and debug overlay text. Use this to verify the command pipeline works before switching to D3D12.

```c
rsx_null_backend_init(1280, 720, "My Game");  // creates window
// ... game runs ...
rsx_null_backend_pump_messages();              // call each frame
```

### D3D12 Backend

**File:** `libs/video/rsx_d3d12_backend.c` (~700 lines)

Real GPU rendering via Direct3D 12. Current capabilities:

| Feature | Status | Details |
|---------|--------|---------|
| Device creation | **Done** | Feature level 11.0, any D3D12-capable GPU |
| Swap chain | **Done** | Double-buffered, flip-discard, VSync present |
| Clear | **Done** | Clears to RSX clear color (ARGB → float4 RGBA) |
| Root signature | **Done** | Empty (input assembler only, no CBV/SRV) |
| Pipeline state | **Done** | Vertex-colored shader (float3 POSITION + float4 COLOR) |
| Vertex buffer | **Done** | 4MB upload heap, persistently mapped |
| Shader compilation | **Done** | Runtime D3DCompile (vs_5_0 / ps_5_0) |
| Frame sync | **Done** | Fence-based, per-frame command allocators |
| Draw calls | Logging | Logs draw_arrays/draw_indexed, not yet wired to VB |
| Textures | Not started | Need texture upload + SRV creation |
| RSX shaders | Not started | Need NV40 ISA → HLSL translation |
| Depth buffer | Not started | Need depth/stencil texture creation |

**How the D3D12 backend renders a frame:**

```
1. Reset command allocator + command list for current frame
2. Transition render target: PRESENT → RENDER_TARGET
3. Set render target, viewport, scissor
4. ClearRenderTargetView with RSX clear color
5. (future: bind PSO, set vertex buffer, draw)
6. Transition render target: RENDER_TARGET → PRESENT
7. Close + execute command list
8. Present with VSync
9. Signal fence, advance to next frame
```

**Shaders** are compiled at init time from inline HLSL strings using `D3DCompile`. The basic vertex-colored shader is:

```hlsl
// Vertex shader
struct VSInput { float3 pos : POSITION; float4 col : COLOR; };
struct VSOutput { float4 pos : SV_POSITION; float4 col : COLOR; };
VSOutput main(VSInput i) { VSOutput o; o.pos = float4(i.pos, 1.0); o.col = i.col; return o; }

// Pixel shader
float4 main(PSInput i) : SV_TARGET { return i.col; }
```

Additional shader definitions in `rsx_d3d12_shaders.h`: solid color fill, textured quad.

---

## Utility Headers

### Primitive Type Mapping (`rsx_primitives.h`)

Maps RSX primitive types to D3D12 topologies. D3D12 doesn't support triangle fans, quads, or line loops — these need index buffer conversion:

| RSX Primitive | Value | D3D12 | Conversion |
|--------------|-------|-------|------------|
| Points | 1 | POINTLIST | Direct |
| Lines | 2 | LINELIST | Direct |
| Line Loop | 3 | LINESTRIP | Add closing edge index |
| Line Strip | 4 | LINESTRIP | Direct |
| Triangles | 5 | TRIANGLELIST | Direct |
| Triangle Strip | 6 | TRIANGLESTRIP | Direct |
| Triangle Fan | 7 | TRIANGLELIST | Expand: center + edge pairs |
| Quads | 8 | TRIANGLELIST | Split: 2 triangles per quad |
| Quad Strip | 9 | TRIANGLELIST | Expand |

Conversion functions generate index buffers:
- `rsx_convert_triangle_fan(first, count, indices, max)` — (N-2)×3 indices
- `rsx_convert_quads(first, count, indices, max)` — (N/4)×6 indices
- `rsx_convert_line_loop(first, count, indices, max)` — N+1 indices

### Vertex Format Mapping (`rsx_vertex_formats.h`)

Maps RSX vertex attribute types (from `NV4097_SET_VERTEX_DATA_ARRAY_FORMAT`) to DXGI formats for D3D12 input layout descriptors:

| RSX Type | ID | Component Size | DXGI Formats (1/2/3/4 components) |
|----------|-----|---------------|----------------------------------|
| None | 0 | - | Disabled |
| S1 (snorm16) | 1 | 2 bytes | R16_SNORM / R16G16 / - / R16G16B16A16 |
| F (float32) | 2 | 4 bytes | R32_FLOAT / R32G32 / R32G32B32 / R32G32B32A32 |
| SF (float16) | 3 | 2 bytes | R16_FLOAT / R16G16 / - / R16G16B16A16 |
| UB (unorm8) | 4 | 1 byte | R8_UNORM / R8G8 / - / R8G8B8A8 |
| S32K (s16) | 5 | 2 bytes | (same as S1) |
| CMP (packed) | 6 | 4 bytes | Custom (11-11-10 bit) |
| UB256 (uint8) | 7 | 1 byte | R8_UINT / R8G8 / - / R8G8B8A8 |

---

## RSX Shader Translation (Future Work)

The RSX uses a proprietary shader ISA derived from NVIDIA's NV40/NV47 architecture:

### Vertex Programs
- Stored in "transform program memory" on the RSX, loaded via `NV4097_SET_TRANSFORM_PROGRAM_LOAD` (method 0x1E9C).
- Up to 512 instructions per program.
- 4-component vector operations (float4).
- 16 input attributes, 16 output varyings.

### Fragment Programs
- Stored in VRAM or main memory at the address written to `NV4097_SET_SHADER_PROGRAM` (method 0x08E4). The low 2 bits indicate memory location (0=local, 1=main).
- Variable length (terminated by end instruction).
- Operates on interpolated fragment values + textures.

### Translation Strategy

1. **Parse** RSX shader bytecode (NV40 ISA encoding)
2. **Convert** to an intermediate representation (instruction list with typed operands)
3. **Emit** HLSL (for D3D12) or GLSL/SPIR-V (for Vulkan)
4. **Compile** at runtime using `D3DCompile` or `glslang`
5. **Cache** compiled shaders by content hash to avoid recompilation

**Practical approach for initial ports:** Most PS3 games use only 10-50 unique shader programs. Rather than implementing a full NV40 ISA translator, start by:
1. Dumping the raw shader bytecode when `SET_SHADER_PROGRAM` / `SET_TRANSFORM_PROGRAM_LOAD` are called
2. Manually writing equivalent HLSL for each unique shader
3. Building a per-game shader lookup table

**Reference implementations:**
- RPCS3: `rpcs3/Emu/RSX/Program/RSXVertexProgram.cpp` and `RSXFragmentProgram.cpp`
- Mesa NV40: `src/gallium/drivers/nouveau/nv30/` (open-source NV40 shader compiler)
- envytools: NV40 ISA documentation at [envytools.readthedocs.io](https://envytools.readthedocs.io/)

---

## Memory Architecture

The RSX has two memory regions:

| Region | Size | Guest Address | Purpose |
|--------|------|---------------|---------|
| **Local (VRAM)** | 256 MB | Configured at init | Framebuffers, textures, vertex data |
| **Main (mapped)** | Variable | Game-specified | GPU-accessible system memory |

`cellGcmMapMainMemory()` makes a region of main memory visible to the RSX by mapping it into the IO address space. The mapping creates entries in the offset table that `cellGcmAddressToOffset()` uses for translation.

**In ps3recomp**, both regions are in the same flat guest VM address space. The distinction matters for the RSX command processor (which needs to know whether a texture/buffer address is in local or main memory) but not for the host CPU (which can access both via `vm_base + addr`).

---

## Integration Guide (For Game Porters)

### Step 1: Null Backend (Prove Command Flow)

```c
// In your game's main.cpp, before entering the game loop:
rsx_null_backend_init(1280, 720, "My Game");
```

Verify that:
- `cellGcmInit` succeeds (log shows RSX configuration)
- Display buffers are registered
- The game loop calls flip functions

### Step 2: D3D12 Backend (Real Rendering)

```c
// Replace null backend with D3D12:
rsx_d3d12_backend_init(1280, 720, "My Game (D3D12)");
```

The window should show the RSX clear color. Draw calls are logged to stderr.

### Step 3: Identify Draw Patterns

Look at the `[D3D12] draw_arrays(prim=5, first=0, count=36)` log messages:
- `prim=5` = triangles, `prim=6` = triangle strip
- Check which vertex attributes are enabled and what format they use
- Note the draw count — small counts suggest UI elements, large counts suggest 3D geometry

### Step 4: Wire Vertex Upload

Read RSX vertex attribute state from `rsx_state`, translate vertex data from guest memory to the D3D12 vertex buffer, and issue `DrawInstanced`. Use `rsx_to_dxgi_format()` from `rsx_vertex_formats.h` for input layout creation.

### Step 5: Shader Translation

For each unique shader program the game uses:
1. Dump the bytecode (from the guest address in `SET_SHADER_PROGRAM`)
2. Write an equivalent HLSL shader
3. Create a new PSO with the game-specific shader
4. Map the shader address to the PSO in a lookup table

---

## File Reference

| File | Lines | Purpose |
|------|-------|---------|
| `cellGcmSys.c` | 830 | RSX state management HLE (33+ functions) |
| `cellGcmSys.h` | 310 | GCM struct definitions and constants |
| `cellVideoOut.c` | 100 | Video output resolution/mode config |
| `cellResc.c` | 254 | Resolution scaling (16 functions) |
| `rsx_commands.c` | 400 | NV47xx FIFO parser + state tracker |
| `rsx_commands.h` | 300 | Method defines, rsx_state, rsx_backend |
| `rsx_d3d12_backend.c` | 700 | D3D12 rendering (device, PSO, VB, shaders) |
| `rsx_d3d12_shaders.h` | 100 | Built-in HLSL shader strings |
| `rsx_null_backend.c` | 260 | Win32 window + GDI clear |
| `rsx_primitives.h` | 112 | Topology mapping + index conversion |
| `rsx_vertex_formats.h` | 115 | Vertex type → DXGI format mapping |

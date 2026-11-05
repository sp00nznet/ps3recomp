# Fragment depth texture comparisons

The shared draw engine and legacy Metal path specialize referenced texture
units from TEXTURE_ADDRESS zfunc. Native depth formats with alpha-kill off
activate comparison only for controls 1 through 6. NEVER (0), ALWAYS (7),
non-depth textures and alpha-kill keep the existing raw sampling behavior.

TEX compares source z with stored red-channel depth. TXP divides xyz by w
before comparison. The reference is clamped to [0,1], and the scalar comparison
is replicated into all four lanes. Comparisons are stored depth <, ==, <=, >,
!=, and >= the reference in control order (`depth FUNC reference`). That is
the reverse of ARB_shadow's `reference FUNC depth`: a shadow map sampled with
GREATER is lit where the stored occluder depth is beyond the fragment's
reference. The first implementation (#302) used
the ARB_shadow order, which inverted every shadow; #403 has the titles that
settled the direction. Linear sampling compares four texel
centers separately before interpolating visibility; it does not compare a
filtered depth value. The existing sampler supplies address/border behavior.

Depth snapshots are currently bound as color textures (R32Float for D24_D8,
R16 UNORM for D16). Explicit HLSL comparisons preserve those bindings rather
than requiring a native depth texture and comparison sampler. UN coordinates
use the bound texture's dimensions. Only referenced active units add shader
helpers and cache identity, avoiding unused texture bindings/readbacks.

Supported comparison scope is 2D D24_D8 or D16, one mip level, TEX/TXP and
matching nearest or linear min/mag filtering. Active float-depth formats,
cubes/other dimensions, multiple mip levels, mismatched filters and
TXB/TXL/TXD/TEXBEM/TXPBEM reject translation with a bounded diagnostic. The
engine drops failed pipelines; the legacy Metal path also rejects these
failures instead of its ordinary built-in shader fallback. Other standalone
legacy backend paths still need to adopt the metadata API. Texture remapping,
LOD/aniso extensions and full hardware shadow sampling precision are outside
this correction's coverage; existing texture/sampler behavior is retained.

Synthetic tests contain no title programs or data:

- `test_shadow_metadata.c`: activation, ordinary HLSL retention, unsupported
  layouts/fetches, sixteen-unit bounded output.
- `test_shadow_engine.c`: production method dispatch, per-unit shader cache,
  switchback, unused-unit bindings and rejected draw emission.
- `test_shadow_compare_metal.m`: actual translated GPU comparisons, projected
  reference, equality, raw endpoint controls and checked legacy failures.

Contract references:

- RPCS3 RSXTexture.cpp: ADDRESS bits 28..31 select zfunc.
  https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/RSXTexture.cpp
- RPCS3 RSXThread.cpp: depth formats, alpha-kill and strict endpoint activation.
  https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/RSXThread.cpp
- RPCS3 FragmentProgramDecompiler.cpp: TEX/TXP shadow reference and replication.
  https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Program/FragmentProgramDecompiler.cpp
- RPCS3 VKDraw.cpp: the sampler compare op is `get_compare_func(zfunc, true)`,
  the reversed function, so `reference OP texel` becomes `depth FUNC reference`.
  https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/VK/VKDraw.cpp
- OpenGL ARB_shadow: reference clamp and comparison before filtering. Its
  comparison direction is the reverse of the RSX's (see above).
  https://registry.khronos.org/OpenGL/extensions/ARB/ARB_shadow.txt

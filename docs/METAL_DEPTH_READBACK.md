# Metal depth readback

The draw engine can store a GPU depth attachment into guest memory when a
texture reinterprets those bytes. For example, a D24S8 attachment can be read
as A8R8G8B8 to reconstruct positions. Metal previously left the optional
`depth_readback` callback unset, so this path sampled the existing guest bytes.

The callback resolves the full depth attachment through the existing R32Float
snapshot shader, then blits the requested rectangle into a shared buffer. The
buffer is kept between calls and grown to the largest copy seen; each call
waits for its blit before returning, so one buffer serves every call. It
checks command-buffer completion and errors before copying floats to the
caller. The full-source resolve preserves offset rectangle coordinates.
Output padding is left untouched. Invalid requests and allocation/submission
failures return an error; temporary snapshot handles retain the existing
record-stream ownership rules.

The engine's depth packing, texture selection, component remap and depth-test
functions are unchanged. The standalone upstream backend also records whether
its synchronous submission completed successfully. The native port uses its
existing completion certificate.

## Optional GPU regression

On a macOS host with an accessible Metal device, from the repository root:

```sh
clang -fobjc-arc -I include -I libs/video \
  libs/video/tests/test_metal_depth_readback.m \
  libs/video/rsx_texture_layout.c libs/video/rsx_shader_msl.cpp -lc++ \
  -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore \
  -o /tmp/test_metal_depth_readback
/tmp/test_metal_depth_readback
```

The 37 checks write nonuniform synthetic GPU depth, preserve the existing
snapshot path, read full and offset rectangles with padded rows, reject invalid
requests without output or queue changes, and reconstruct depth on the GPU
from canonical D24S8 bytes through the actual A8R8G8B8 decoder and component
remap. They also verify a queued clear and temporary-object cleanup. All inputs
are synthetic. No window, drawable, original game program or asset is used.

The existing platform-neutral draw-engine suite separately verifies the engine
stores depth 0.5 as `80 00 00 00` and samples a texture decoded from those bytes.
The GPU fixture's canonical byte construction is independent of that packing
implementation; this change does not modify it.

Shared-buffer access waits for scheduled GPU writes, as required by
[Apple's shared-storage documentation](https://developer.apple.com/documentation/metal/mtlresourceoptions/storagemodeshared).
The texture-to-buffer operation uses
[Metal's blit encoder](https://developer.apple.com/documentation/metal/mtlblitcommandencoder).

## Encoding failure regression

`test_metal_depth_readback_errors.m` runs the nominal 37 checks, then injects
nil resolve and pending-clear render encoders and verifies that a completed
command buffer does not certify skipped rendering. It also tests a clear that
fails during the engine's preceding `submit_and_wait`, followed by a fresh
readback submission, plus invalid pipelines and missing attachments.
All 97 checks verify error returns, the whole output including row padding,
and temporary snapshot cleanup. Build using the command above, substituting
this test filename.

Encoding failures remain sticky until backend reinitialization or shutdown.
The generic certificate marks existing skipped-work paths without replacing
passes or changing their skip behavior. The native callback checks its existing
atomic failure certificate in addition to command-buffer completion.

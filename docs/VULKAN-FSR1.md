# Optional FSR1 spatial upscaling

`PS3RECOMP_UPSCALER=fsr1` enables AMD EASU followed by RCAS at 0.2 stops.
`off` is the default; invalid values fail initialization. The register-file
draw engine passes the original guest display surface to EASU before the
ordinary display-size blit. The RCAS result is copied at matching dimensions
to the host display target. Optional FXAA runs afterward. Readback and frame
dumps include both filters, and window presentation uses this same result.

The diagnostic fixed-function vtable path reports that FSR1 is bypassed.
Downscaling or a non-display-UNORM guest source uses the original display blit
and logs the bypass. An allocation, capability or execution failure reports
fallback to the original blit for the remainder of the session. A requested
setting therefore is not evidence that FSR1 rendered successfully. Successful
frames log the actual input and output dimensions.

Two private RGBA8 targets preserve guest surfaces. Dimensions, format features
and push-constant limits are checked; a changed source or output recreates the
resources after synchronous submissions finish. EASU and RCAS draw in one
submission with explicit image dependencies. Both passes clean up partial
allocation failures. FSR off allocates neither target.

This filters the complete guest display, including UI. UI separation and game
quality remain pending. Internal render scale is independent; a guest surface
larger than the requested display is downscaled normally rather than using FSR.
Resizing the host window scales the configured display result; it does not
change the startup output-size setting. There is no temporal reconstruction.

The synthetic Vulkan test executes both passes and the production display path
on explicitly selected CPU lavapipe. It checks constants, edges, bilinear alpha,
RCAS alpha preservation, repeatability, source binding changes, resize, FXAA
composition and injected allocation-failure fallback. The display result must
match direct EASU/RCAS output and differ from ordinary linear enlargement.
Physical GPU/window presentation, matched game scenes, UI quality, frame times,
Android and Steam Deck execution remain separate acceptance work.

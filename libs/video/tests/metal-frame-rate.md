Set `PS3RECOMP_METAL_FPS=1` for a windowed macOS run. The original title is
preserved and gains a once-per-second display FPS, guest submission FPS,
average/p95/maximum display interval in milliseconds, and seconds since the
last displayed frame. The same fields are emitted as `[RSX FPS]` log lines.

Display FPS counts `MTLDrawable` presentation callbacks with a positive,
increasing `presentedTime`. Skipped drawables, GPU flushes, and headless
submissions do not count. Host boot redraws can count as display frames; they
are kept separate from guest submissions. Hosts should call
`rsx_metal_backend_note_guest_frame()` once at each guest FLIP submission.
Without that hook, guest FPS stays zero.

FPS is presentations/submissions divided by actual wall-clock sample duration.
Intervals retain continuity across sample boundaries, so a resumed frame
exposes the preceding stall. A sample with no new frames reports zero FPS and
zero interval statistics, while age keeps increasing. The p95 uses nearest
rank over at most the most recent 256 intervals in the sample; average and
maximum include every interval. The first frame has no preceding interval.

The main queue timer updates independently of the frame submission thread.
If the main queue itself blocks, the title cannot update; log timestamps make
that gap visible. No image readback, GPU wait, rendering change, or per-frame
statistics allocation is introduced. Drawable callbacks retain their own
session so shutdown/reinitialization cannot contaminate a new run.

Run `python3 tools/test_metal_frame_rate.py` on macOS with a display. It checks
deterministic rates/stalls/duplicate timestamps/bounded p95, worker-thread
headless shutdown without main-queue pumping, a native title
timer without a rendering thread, and disabled mode. A deterministic drawable
substitute invokes the production registration/callback with positive, zero,
duplicate, stale, and non-finite timestamps, and checks late callbacks after
session replacement.

The real drawable scenario requires six GPU clears to complete without error,
reads back their expected pixel, and compares production display counters and
the last timestamp to an independent observer on the same Metal drawables.
An active display with an identified I/O Kit monitor vendor/model must produce
a positive presentation. This identity check does not prove physical scanout.
Other displays must satisfy the same GPU and timestamp-parity assertions; zero
observed timestamps require exactly zero display frames and explicitly report
that actual presentation was unobserved. The hosted macOS runner's
paravirtualized GPU was observed to deliver either no callbacks or callbacks
with all-zero timestamps despite an active display and visible fixture window.
Neither GPU completion nor that window visibility is counted as a display frame.

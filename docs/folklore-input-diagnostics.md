# Controller evidence

`tools/pad_sdl_probe.c` is a standalone, bounded SDL2 console probe. It reads
controller input without launching a game, opening a window, or using the GPU.
It does not change runtime deadzones, mappings, saves, or calibration.

Build with the same SDL2 library used by the game:

```text
clang -std=c11 -Wall -Wextra -Werror tools/pad_sdl_probe.c -lSDL2 -o pad_sdl_probe.exe
pad_sdl_probe.exe --seconds 20 > axes.csv
```

Hold each full-stick direction, then rotate the stick around its edge and
release it. CSV contains raw SDL axes and the exact current cellPad byte
conversion. This can establish whether a full-stick hold fluctuates at the
host, but it does not by itself prove the game's run/walk threshold or behavior.
Button bits in this diagnostic are SDL button indices, not PS3 masks.

Add `--motion` only to inspect physical sensor data. The probe reports gyro
and accelerometer capability independently and publishes converted PS3 motion
only when both sensors can be enabled and read through the production adapter.
No controller motion is synthesized. A controller's vendor gyro-to-stick mode
does not establish raw SDL gyro support. Sensor enablement is restored on normal
exit, including partial enable failures. No persistent settings are written.

Duration is limited to 1–120 seconds and sample interval to 4–1000 milliseconds
(`--interval-ms`, default 16). Identity/capability messages go to stderr, CSV
to stdout. Exit 3 means no SDL game controller was available, not a successful
physical test. Exit 2 means invalid arguments; exit 1 means initialization or
output failure.

`libs/input/tests/test_pad_sdl_probe_virtual.c` exercises the actual tool with
SDL's process-local virtual joystick. It verifies axis transport and endpoints;
it is not physical gyro or in-game capture evidence.

#!/usr/bin/env bash
#
# Package the POSIX host as ps3recomp.app and wrap it in a drag-to-install DMG.
#
#   ./packaging/macos/make_dmg.sh [build-dir] [output.dmg]
#
# The bundle is ad-hoc signed, which is enough to run locally. It is NOT
# notarised: distributing it would need an Apple Developer ID, so a downloader
# gets Gatekeeper's "unidentified developer" prompt. Right-click > Open, or
# `xattr -dr com.apple.quarantine /Applications/ps3recomp.app`, clears it.
set -euo pipefail

BUILD_DIR="${1:-build}"
DMG_OUT="${2:-ps3recomp-macos-arm64.dmg}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
APP_NAME="ps3recomp.app"
VOL_NAME="ps3recomp"

BIN="$ROOT/$BUILD_DIR/ps3recomp_host"
[ -x "$BIN" ] || { echo "error: $BIN not found -- build first:"; \
                   echo "  cmake -S . -B $BUILD_DIR -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR"; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
APP="$STAGE/$APP_NAME"

echo "==> building $APP_NAME"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN"                     "$APP/Contents/MacOS/ps3recomp_host"
cp "$HERE/Info.plist"         "$APP/Contents/Info.plist"
cp "$HERE/ps3recomp.icns"     "$APP/Contents/Resources/ps3recomp.icns"
printf 'APPL????' > "$APP/Contents/PkgInfo"

# Launched from Finder there are no argv, so the demo needs its flags baked in.
mv "$APP/Contents/MacOS/ps3recomp_host" "$APP/Contents/MacOS/ps3recomp_bin"
cat > "$APP/Contents/MacOS/ps3recomp_host" <<'LAUNCH'
#!/bin/sh
exec "$(dirname "$0")/ps3recomp_bin" --draw --frames=0 "$@"
LAUNCH
chmod +x "$APP/Contents/MacOS/ps3recomp_host"

echo "==> ad-hoc signing"
codesign --force --deep --sign - "$APP" 2>/dev/null || echo "    (codesign unavailable; bundle left unsigned)"

echo "==> staging DMG contents"
ln -s /Applications "$STAGE/Applications"
cat > "$STAGE/README.txt" <<'README'
ps3recomp - macOS host
======================

Drag ps3recomp.app onto the Applications folder to install.

What this is
------------
A native Apple Silicon build of the ps3recomp host and its Metal RSX backend.
Launching it opens a window driven by a real NV4097 command buffer: the guest
clear colour, and a triangle whose vertices are fetched out of emulated PS3
guest memory as big-endian data. Close the window to quit.

What this is NOT
----------------
This does not run PS3 games. No PS3 title renders gameplay under ps3recomp on
any platform yet. This is the graphics bring-up harness that backend work is
developed against -- it needs no game binary, which is precisely the point.

Command line
------------
The binary inside the bundle takes flags:

  ps3recomp.app/Contents/MacOS/ps3recomp_bin [--draw] [--frames=N]

  --draw       issue a real DRAW_ARRAYS as well as the clear
  --frames=N   run N frames (0 = until the window is closed)

  PS3RECOMP_METAL_HEADLESS=1 renders offscreen and asserts the presented
  pixel, exiting non-zero on mismatch. That is what CI runs.

The app is ad-hoc signed, not notarised. If macOS blocks it, right-click the
app and choose Open, or run:
  xattr -dr com.apple.quarantine /Applications/ps3recomp.app

https://github.com/sp00nznet/ps3recomp
README

echo "==> creating $DMG_OUT"
rm -f "$ROOT/$DMG_OUT"
hdiutil create -volname "$VOL_NAME" -srcfolder "$STAGE" -ov -format UDZO \
               -quiet "$ROOT/$DMG_OUT"

echo "==> done: $DMG_OUT ($(du -h "$ROOT/$DMG_OUT" | cut -f1))"

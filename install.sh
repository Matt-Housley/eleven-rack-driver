#!/bin/bash
# install.sh — build and install the Eleven Rack user-space audio driver.
#
# This installs a CoreAudio HAL plugin (a signed .driver bundle) into
# /Library/Audio/Plug-Ins/HAL and builds the user-space USB engine. NO kernel
# extension, NO DriverKit system extension, and NO "Reduced Security" / Recovery
# changes are required — a HAL plugin is loaded by coreaudiod with SIP fully on.
# The only privileged steps are copying the bundle into /Library and restarting
# coreaudiod, which need your admin password once.
#
# Result: an 8-in / 6-out "Eleven Rack" device appears in Audio MIDI Setup once
# you run the engine (build/erengine), which owns the USB device and bridges
# audio to the plugin through a shared-memory ring.

set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_SRC="$DIR/ElevenRackAudioPlugin/ElevenRackAudioPlugin.cpp"
INFO="$DIR/ElevenRackAudioPlugin/Info.plist"
ENGINE_SRC="$DIR/ElevenRackBridge/erengine.c"
BUILD="$DIR/build"
DRIVER="$BUILD/Eleven Rack.driver"
HAL="/Library/Audio/Plug-Ins/HAL"

echo "==> Building HAL plugin bundle"
rm -rf "$BUILD"; mkdir -p "$DRIVER/Contents/MacOS"
cp "$INFO" "$DRIVER/Contents/Info.plist"
clang++ -std=c++17 -O2 -bundle "$PLUGIN_SRC" \
    -o "$DRIVER/Contents/MacOS/ElevenRackAudioPlugin" \
    -framework CoreAudio -framework CoreFoundation -framework AudioToolbox \
    -Wno-deprecated-declarations

echo "==> Ad-hoc code-signing the plugin (required for coreaudiod to load it)"
codesign --force --deep --sign - "$DRIVER"
codesign --verify --verbose "$DRIVER" || { echo "signing failed"; exit 1; }

echo "==> Building the user-space USB engine"
clang -O2 -o "$BUILD/erengine" "$ENGINE_SRC" \
    -framework IOKit -framework CoreFoundation -framework CoreMIDI -framework AudioToolbox \
    -framework CoreAudio -Wno-deprecated-declarations
echo "    built $BUILD/erengine"

echo "==> Installing plugin to $HAL (admin password required)"
sudo mkdir -p "$HAL"
sudo rm -rf "$HAL/Eleven Rack.driver"
sudo cp -R "$DRIVER" "$HAL/"
sudo chown -R root:wheel "$HAL/Eleven Rack.driver"

echo "==> Restarting coreaudiod to load the plugin"
sudo killall coreaudiod 2>/dev/null || true

cat <<EOF

Done. The plugin is installed (no drivers, no security changes).

Next:
  1. Plug in the Eleven Rack.
  2. Start the engine (owns the USB device, bridges audio to the plugin):
         "$BUILD/erengine"
     Leave it running. (You can background it or wrap it in a LaunchAgent later.)
  3. Open Audio MIDI Setup — an 8-in / 6-out "Eleven Rack" device should appear,
     and CoreMIDI shows the Eleven Rack MIDI endpoints.

To uninstall:
     sudo rm -rf "$HAL/Eleven Rack.driver" && sudo killall coreaudiod
EOF

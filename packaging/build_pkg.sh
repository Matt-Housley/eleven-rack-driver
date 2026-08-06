#!/bin/bash
# build_pkg.sh — build the whole Eleven Rack driver and produce a double-clickable
# installer: dist/Eleven Rack <version>.pkg
#
# This is a DEVELOPER step, run once per release. End users never run any command:
# they double-click the resulting .pkg. It builds and ad-hoc-signs the HAL plugin,
# the USB engine, and the Swift menu-bar app, stages the install layout, then wraps
# it with pkgbuild + productbuild.
#
# Signing: ad-hoc ("-") by default. To ship a notarized, warning-free installer,
# set SIGN_ID to a "Developer ID Application" identity and PKG_SIGN_ID to a
# "Developer ID Installer" identity (and notarize the output with notarytool).

set -euo pipefail

# ---- configuration ---------------------------------------------------------
VERSION="${ER_VERSION:-1.0.0}"     # override via ER_VERSION (the release workflow passes the tag)
SIGN_ID="${SIGN_ID:--}"            # code signing identity ("-" = ad-hoc)
PKG_SIGN_ID="${PKG_SIGN_ID:-}"     # installer signing identity ("" = unsigned)
IDENTIFIER="co.uk.matthousley.ElevenRack.pkg"
DEPLOY_TARGET="arm64-apple-macosx13.0"

# ---- paths -----------------------------------------------------------------
PKG_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$PKG_DIR/.." && pwd)"
PLUGIN_SRC="$ROOT/ElevenRackAudioPlugin/ElevenRackAudioPlugin.cpp"
PLUGIN_INFO="$ROOT/ElevenRackAudioPlugin/Info.plist"
ENGINE_SRC="$ROOT/ElevenRackBridge/erengine.c"
BRIDGE_DIR="$ROOT/ElevenRackBridge"
APP_SRC="$ROOT/ElevenRackApp"

# Stage outside the repo (which lives under ~/Downloads, a TCC-protected folder).
# Keeps root-owned Installer artifacts — should a relocation ever occur — out of a
# location that is awkward to clean, and off Spotlight's app index.
BUILD="${ER_BUILD_DIR:-/private/tmp/ElevenRack-build}"
STAGE="$BUILD/root"
DIST="$ROOT/dist"
COMPONENT="$BUILD/ElevenRackComponent.pkg"
PRODUCT="$DIST/Eleven Rack $VERSION.pkg"

APP="$STAGE/Applications/Eleven Rack.app"
DRIVER="$STAGE/Library/Audio/Plug-Ins/HAL/Eleven Rack.driver"
AGENTS="$STAGE/Library/LaunchAgents"

echo "==> Cleaning $BUILD"
rm -rf "$BUILD" 2>/dev/null || true
# Fail loudly rather than silently reuse a stale build: a prior install could have
# relocated a root-owned copy into the staging dir that this (non-root) build can't
# overwrite. If so, the leftovers survive the rm and every rebuild ships stale bits.
if [ -e "$BUILD" ]; then
    echo "ERROR: could not remove '$BUILD' — it contains files this user can't delete"
    echo "       (root-owned leftovers from a prior install relocation)."
    echo "       Run once, then rebuild:  sudo rm -rf \"$BUILD\""
    exit 1
fi
mkdir -p "$STAGE" "$DIST"
# Keep Spotlight from indexing the staged app bundle, so it can never become an
# Installer relocation target (belt-and-suspenders with BundleIsRelocatable=false).
touch "$BUILD/.metadata_never_index"
mkdir -p "$DRIVER/Contents/MacOS" "$APP/Contents/MacOS" "$APP/Contents/Resources" "$AGENTS"

# ---- 1. HAL plugin bundle --------------------------------------------------
echo "==> Building HAL plugin"
cp "$PLUGIN_INFO" "$DRIVER/Contents/Info.plist"
clang++ -std=c++17 -O2 -bundle "$PLUGIN_SRC" \
    -o "$DRIVER/Contents/MacOS/ElevenRackAudioPlugin" \
    -I "$BRIDGE_DIR" \
    -framework CoreAudio -framework CoreFoundation -framework AudioToolbox \
    -Wno-deprecated-declarations
codesign --force --sign "$SIGN_ID" "$DRIVER"

# ---- 2. USB engine ---------------------------------------------------------
echo "==> Building USB engine"
clang -O2 -o "$APP/Contents/Resources/erengine" "$ENGINE_SRC" \
    -I "$BRIDGE_DIR" \
    -framework IOKit -framework CoreFoundation -framework CoreMIDI \
    -framework AudioToolbox -framework CoreAudio -Wno-deprecated-declarations
codesign --force --sign "$SIGN_ID" "$APP/Contents/Resources/erengine"

# ---- 3. Menu-bar app -------------------------------------------------------
echo "==> Building menu-bar app"
cp "$APP_SRC/Info.plist" "$APP/Contents/Info.plist"
# Stamp the build version so the app's About/Finder version matches the package.
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$APP/Contents/Info.plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VERSION" "$APP/Contents/Info.plist" 2>/dev/null || true
cp "$APP_SRC/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
cp "$APP_SRC/Assets/MenuBarIcon-master.png" "$APP/Contents/Resources/MenuBarIcon.png"
/usr/bin/swiftc -O -o "$APP/Contents/MacOS/ElevenRack" \
    "$APP_SRC"/*.swift \
    -import-objc-header "$APP_SRC/ElevenRack-Bridging-Header.h" \
    -I "$BRIDGE_DIR" \
    -framework AppKit -framework SwiftUI -framework CoreAudio -framework CoreMIDI \
    -target "$DEPLOY_TARGET"
# Sign the app last (nested engine already signed).
codesign --force --deep --sign "$SIGN_ID" "$APP"
codesign --verify --deep --strict "$APP"

# ---- 4. LaunchAgent --------------------------------------------------------
echo "==> Staging LaunchAgent"
cp "$PKG_DIR/com.matthousley.ElevenRack.plist" "$AGENTS/co.uk.matthousley.ElevenRack.plist"

# ---- 5. Component pkg ------------------------------------------------------
echo "==> pkgbuild"
# Strip removable extended attributes (e.g. quarantine). The system-managed
# com.apple.provenance xattr cannot be removed and is harmless in the payload.
find "$STAGE" -exec xattr -c {} + 2>/dev/null || true
chmod +x "$PKG_DIR/scripts/postinstall"

# Force the bundles non-relocatable. Otherwise Installer may relocate the app to
# an existing copy of the same bundle id that Spotlight has indexed (e.g. this
# build's staging folder) instead of installing it to /Applications.
COMPONENT_PLIST="$BUILD/component.plist"
pkgbuild --analyze --root "$STAGE" "$COMPONENT_PLIST"
python3 - "$COMPONENT_PLIST" <<'PY'
import plistlib, sys
with open(sys.argv[1], "rb") as f: items = plistlib.load(f)
for it in items:
    if isinstance(it, dict): it["BundleIsRelocatable"] = False
with open(sys.argv[1], "wb") as f: plistlib.dump(items, f)
PY

pkgbuild --root "$STAGE" \
    --identifier "$IDENTIFIER" \
    --version "$VERSION" \
    --scripts "$PKG_DIR/scripts" \
    --component-plist "$COMPONENT_PLIST" \
    --ownership recommended \
    --install-location / \
    "$COMPONENT"

# ---- 6. Product (distribution) pkg ----------------------------------------
echo "==> productbuild"
productbuild --distribution "$PKG_DIR/distribution.xml" \
    --resources "$PKG_DIR/resources" \
    --package-path "$BUILD" \
    "$PRODUCT"

# ---- 7. Optional installer signing ----------------------------------------
if [ -n "$PKG_SIGN_ID" ]; then
    echo "==> Signing installer with '$PKG_SIGN_ID'"
    productsign --sign "$PKG_SIGN_ID" "$PRODUCT" "$PRODUCT.signed"
    mv "$PRODUCT.signed" "$PRODUCT"
fi

echo
echo "Built: $PRODUCT"
[ "$SIGN_ID" = "-" ] && echo "(ad-hoc signed — users approve once via right-click → Open / Privacy & Security)"

#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$SCRIPT_DIR/build"
APP_DIR="$OUT_DIR/IslamwareLoader.app"

rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources"

echo "[1/3] Compiling IslamwareLoader (SwiftUI/AppKit)..."
swiftc -O \
    -framework SwiftUI \
    -framework AppKit \
    -framework UniformTypeIdentifiers \
    "$SCRIPT_DIR/IslamwareLoader.swift" \
    -o "$APP_DIR/Contents/MacOS/IslamwareLoader"

echo "[2/3] Writing Info.plist & bundling resources..."
cat > "$APP_DIR/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>IslamwareLoader</string>
    <key>CFBundleIdentifier</key>
    <string>com.islamware.v34loader</string>
    <key>CFBundleName</key>
    <string>IslamwareLoader</string>
    <key>CFBundleDisplayName</key>
    <string>Islamware Loader</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>12.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSMicrophoneUsageDescription</key>
    <string>Counter-Strike: Source needs microphone access for in-game voice chat and audio initialization.</string>
    <key>NSAppTransportSecurity</key>
    <dict>
        <key>NSAllowsArbitraryLoads</key>
        <true/>
    </dict>
</dict>
</plist>
PLIST

cp "$SCRIPT_DIR/setup_v34.sh" "$APP_DIR/Contents/Resources/setup_v34.sh"
chmod +x "$APP_DIR/Contents/Resources/setup_v34.sh" "$SCRIPT_DIR/setup_v34.sh"

# Bundle built v34 DLL and injector if available so the .app is self-contained
if [[ -f "$REPO_DIR/build/islamware_v34.dll" ]]; then
    cp "$REPO_DIR/build/islamware_v34.dll" "$APP_DIR/Contents/Resources/islamware_v34.dll"
fi
if [[ -f "$REPO_DIR/build/injector_v34.exe" ]]; then
    cp "$REPO_DIR/build/injector_v34.exe" "$APP_DIR/Contents/Resources/injector_v34.exe"
fi

echo "[3/3] Done! Built: $APP_DIR"

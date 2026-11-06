#!/bin/sh
# Builds BluePixel.app into ./build
set -e
cd "$(dirname "$0")/.."
swift build -c release --product BluePixel
APP=build/BluePixel.app
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp .build/release/BluePixel "$APP/Contents/MacOS/BluePixel"
# App icon: Resources/AppIcon.png (1024x1024, transparent margins) -> AppIcon.icns
ICONSET="$(mktemp -d)/AppIcon.iconset"; mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
  sips -z $s $s Resources/AppIcon.png --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
  sips -z $((s*2)) $((s*2)) Resources/AppIcon.png --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>BluePixel</string>
<key>CFBundleDisplayName</key><string>BluePixel</string>
<key>CFBundleIdentifier</key><string>net.bluepixel.BluePixel</string>
<key>CFBundleExecutable</key><string>BluePixel</string>
<key>CFBundleIconFile</key><string>AppIcon</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.1</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>NSPrincipalClass</key><string>NSApplication</string>
<key>CFBundleDocumentTypes</key><array><dict>
<key>CFBundleTypeName</key><string>Amstrad CPC media</string>
<key>CFBundleTypeRole</key><string>Viewer</string>
<key>CFBundleTypeExtensions</key><array><string>dsk</string><string>sna</string><string>cdt</string><string>tzx</string><string>voc</string><string>cpr</string><string>crt</string><string>wpf</string></array>
</dict></array>
</dict></plist>
PLIST
codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || true
echo "$APP"

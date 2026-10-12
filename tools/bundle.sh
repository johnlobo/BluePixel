#!/bin/sh
# Builds MacAPE.app into ./build
set -e
cd "$(dirname "$0")/.."
swift build -c release --product MacAPE
APP=build/MacAPE.app
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp .build/release/MacAPE "$APP/Contents/MacOS/MacAPE"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>MacAPE</string>
<key>CFBundleDisplayName</key><string>MacAPE</string>
<key>CFBundleIdentifier</key><string>net.macape.MacAPE</string>
<key>CFBundleExecutable</key><string>MacAPE</string>
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

#!/bin/sh
# Build "TM-T20III Utility.app" (universal arm64 + x86_64) from the Swift package.
#
#   SIGN_ID   codesign identity. Default "-" = ad-hoc (fine for your own Mac).
#             For distribution: "Developer ID Application: Your Name (TEAMID)";
#             the app is then signed with the hardened runtime + secure timestamp,
#             ready for notarization (scripts/make-installer.sh does this).
#   ID_PREFIX bundle identifier prefix (default local.tmt20iii)
set -e
cd "$(dirname "$0")/.."
SIGN_ID="${SIGN_ID:--}"
ID_PREFIX="${ID_PREFIX:-local.tmt20iii}"
VERSION=$(cat ../VERSION 2>/dev/null || echo 0.0.0)
BUILD_NUM=$(git rev-list --count HEAD 2>/dev/null || echo 1)

swift build -c release --arch arm64 --arch x86_64 --product TMUtility
BIN=$(swift build -c release --arch arm64 --arch x86_64 --show-bin-path)/TMUtility

APP="TM-T20III Utility.app"
rm -rf "$APP" TMUtility.app
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/TMUtility"
cp Resources/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>TM-T20III Utility</string>
  <key>CFBundleDisplayName</key><string>TM-T20III Utility</string>
  <key>CFBundleIdentifier</key><string>${ID_PREFIX}.utility</string>
  <key>CFBundleExecutable</key><string>TMUtility</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${VERSION}</string>
  <key>CFBundleVersion</key><string>${BUILD_NUM}</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSAppTransportSecurity</key><dict><key>NSAllowsLocalNetworking</key><true/></dict>
</dict>
</plist>
EOF

if [ "$SIGN_ID" = "-" ]; then
    codesign --force --sign - "$APP"                 # ad-hoc (required on Apple Silicon)
else
    codesign --force --options runtime --timestamp --sign "$SIGN_ID" "$APP"
fi
codesign --verify --strict "$APP"
echo "Built $(pwd)/$APP ($VERSION, signed with: $SIGN_ID)"

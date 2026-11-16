#!/bin/bash
# Build, sign, notarize and staple the downloadable installer:
#     dist/TM-T20III-Driver-<VERSION>.pkg  (+ .sha256)
#
# One-time setup (see README "Making the signed installer"):
#   - "Developer ID Application" and "Developer ID Installer" certificates in your login keychain
#   - xcrun notarytool store-credentials tmt20iii-notary --apple-id YOU --team-id TEAMID
#
# Usage: scripts/make-installer.sh [--unsigned] [--no-notarize] [--skip-tests] [--release]
#   --unsigned     ad-hoc signed binaries, unsigned pkg (local testing only)
#   --no-notarize  sign but skip notarization/stapling
#   --skip-tests   don't run the test suites before packaging
#   --release      upload the pkg to a GitHub release v<VERSION> (needs `gh` and a pushed repo)
#
# Environment overrides:
#   DEV_ID_APP        pick among several Developer ID Application certs (name, team ID or SHA-1)
#   DEV_ID_INSTALLER  same for the Developer ID Installer cert     (both auto-detected)
#   NOTARY_PROFILE    notarytool keychain profile (default tmt20iii-notary)
#   ID_PREFIX         bundle / package identifier prefix (default local.tmt20iii)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOTDIR=$(pwd)

SIGN=1 NOTARIZE=1 TESTS=1 RELEASE=0
for a in "$@"; do
    case "$a" in
        --unsigned) SIGN=0 NOTARIZE=0 ;;
        --no-notarize) NOTARIZE=0 ;;
        --skip-tests) TESTS=0 ;;
        --release) RELEASE=1 ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "unknown option $a" >&2; exit 2 ;;
    esac
done

[ "$(uname -s)" = Darwin ] || { echo "make-installer.sh must run on macOS" >&2; exit 1; }
VERSION=$(cat VERSION)
ID_PREFIX="${ID_PREFIX:-local.tmt20iii}"
PKG_ID="$ID_PREFIX.driver"
NOTARY_PROFILE="${NOTARY_PROFILE:-tmt20iii-notary}"
OUT="dist/TM-T20III-Driver-$VERSION.pkg"

step() { printf '\n==> %s\n' "$*"; }
# identity KIND [FILTER] -> "SHA1<TAB>full name" of the first valid identity whose name starts
# with KIND ("Developer ID Application" / "Developer ID Installer") and contains FILTER
# (a name, team ID or SHA-1). Only Developer ID certificates can sign software distributed
# outside the App Store; "Apple Distribution"/"Apple Development" ones are never picked.
identity() {
    security find-identity -v 2>/dev/null | awk -F'"' -v k="$1" -v f="${2:-}" '
        index($2, k) == 1 && (f == "" || index($2, f) || index(toupper($1), toupper(f))) {
            split($1, a, " "); print a[2] "\t" $2; exit }'
}
no_identity() {
    echo "error: no valid '$1' certificate${2:+ matching '$2'} in the keychain." >&2
    echo "Identities found (only 'Developer ID ...' ones work for a downloadable pkg):" >&2
    security find-identity -v >&2 || true
    echo "Create it in Xcode > Settings > Accounts > Manage Certificates > + > $1." >&2
    exit 1
}

if [ $SIGN = 1 ]; then
    app=$(identity "Developer ID Application" "${DEV_ID_APP:-}")
    [ -n "$app" ] || no_identity "Developer ID Application" "${DEV_ID_APP:-}"
    inst=$(identity "Developer ID Installer" "${DEV_ID_INSTALLER:-}")
    [ -n "$inst" ] || no_identity "Developer ID Installer" "${DEV_ID_INSTALLER:-}"
    TAB=$(printf '\t')
    DEV_ID_APP=${app%%"$TAB"*}                 # sign by SHA-1: no ambiguity between certificates
    DEV_ID_INSTALLER=${inst#*"$TAB"}
    echo "App signing:       ${app#*"$TAB"} ($DEV_ID_APP)"
    echo "Installer signing: $DEV_ID_INSTALLER"

    # "unable to build chain to self-signed root" / errSecInternalComponent = Apple's
    # intermediate certificate is missing (or the keychain is locked, e.g. over SSH).
    if ! security find-certificate -c "Developer ID Certification Authority" >/dev/null 2>&1; then
        echo "error: Apple's 'Developer ID Certification Authority' intermediate certificate is not installed." >&2
        echo "Download and double-click both, then re-run:" >&2
        echo "  https://www.apple.com/certificateauthority/DeveloperIDG2CA.cer" >&2
        echo "  https://www.apple.com/certificateauthority/DeveloperIDCA.cer" >&2
        exit 1
    fi
    probe=$(mktemp -t tmt20sign)
    cp /usr/bin/true "$probe"
    if ! codesign --force --options runtime --timestamp --sign "$DEV_ID_APP" "$probe" 2>"$probe.err"; then
        cat "$probe.err" >&2
        echo "error: test signature failed. If the keychain is locked (SSH session), run:" >&2
        echo "  security unlock-keychain ~/Library/Keychains/login.keychain-db" >&2
        rm -f "$probe" "$probe.err"
        exit 1
    fi
    rm -f "$probe" "$probe.err"
else
    DEV_ID_APP="-"
    echo "Unsigned build (ad-hoc binaries, unsigned pkg) - for local testing only."
fi

step "Building driver + tools (universal)"
make clean >/dev/null
make
make -C utility clean >/dev/null
make -C utility tmutil tmqueue
if [ $TESTS = 1 ]; then
    step "Running tests"
    make test
    make filter-test
    make -C utility test
fi

step "Building TM-T20III Utility.app"
SIGN_ID="$DEV_ID_APP" ID_PREFIX="$ID_PREFIX" utility/scripts/make-app.sh

step "Staging payload"
WORK=$(mktemp -d -t tmt20pkg)
trap 'rm -rf "$WORK"' EXIT
R="$WORK/root"
install -d "$R/Library/Printers/Epson/TMT20III" "$R/Library/Printers/PPDs/Contents/Resources" \
           "$R/Applications" "$R/usr/local/bin"
install -m 755 build/rastertotmt20 "$R/Library/Printers/Epson/TMT20III/rastertotmt20"
install -m 755 scripts/add-printer.sh "$R/Library/Printers/Epson/TMT20III/add-printer.sh"
install -m 755 scripts/uninstall.sh "$R/Library/Printers/Epson/TMT20III/uninstall.sh"
sed "s/^\*FileVersion: .*/*FileVersion: \"$VERSION\"/" ppd/EpsonTMT20III.ppd \
    > "$R/Library/Printers/PPDs/Contents/Resources/EpsonTMT20III.ppd"   # PPD *FileVersion = VERSION
chmod 644 "$R/Library/Printers/PPDs/Contents/Resources/EpsonTMT20III.ppd"
install -m 755 utility/build/tmutil utility/build/tmqueue "$R/usr/local/bin/"
ditto "utility/TM-T20III Utility.app" "$R/Applications/TM-T20III Utility.app"

sign_bin() {
    if [ $SIGN = 1 ]; then
        codesign --force --options runtime --timestamp --identifier "$ID_PREFIX.$(basename "$1")" \
                 --sign "$DEV_ID_APP" "$1"
    else
        codesign --force --sign - "$1"
    fi
}
sign_bin "$R/Library/Printers/Epson/TMT20III/rastertotmt20"
sign_bin "$R/usr/local/bin/tmutil"
sign_bin "$R/usr/local/bin/tmqueue"
codesign --verify --strict --deep "$R/Applications/TM-T20III Utility.app"

step "Building package"
mkdir -p "$WORK/scripts" dist
install -m 755 pkg/scripts/postinstall "$WORK/scripts/postinstall"
pkgbuild --analyze --root "$R" "$WORK/component.plist" >/dev/null
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$WORK/component.plist" >/dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$WORK/component.plist"
    i=$((i + 1))
done
pkgbuild --root "$R" --component-plist "$WORK/component.plist" --scripts "$WORK/scripts" \
         --identifier "$PKG_ID" --version "$VERSION" --install-location / \
         "$WORK/TMT20III-component.pkg"
sed -e "s/@PKG_ID@/$PKG_ID/g" -e "s/@VERSION@/$VERSION/g" pkg/distribution.xml > "$WORK/distribution.xml"
rm -f "$OUT"
if [ $SIGN = 1 ]; then
    productbuild --distribution "$WORK/distribution.xml" --resources pkg/resources \
                 --package-path "$WORK" --sign "$DEV_ID_INSTALLER" --timestamp "$OUT"
    pkgutil --check-signature "$OUT"
else
    productbuild --distribution "$WORK/distribution.xml" --resources pkg/resources \
                 --package-path "$WORK" "$OUT"
fi

if [ $NOTARIZE = 1 ]; then
    step "Notarizing (this usually takes a few minutes)"
    xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$OUT"
    spctl --assess --type install -vv "$OUT"
fi

(cd dist && shasum -a 256 "$(basename "$OUT")" > "$(basename "$OUT").sha256")
step "Done: $ROOTDIR/$OUT"
cat "$OUT.sha256"

if [ $RELEASE = 1 ]; then
    [ $NOTARIZE = 1 ] || { echo "Refusing to publish a pkg that is not notarized." >&2; exit 1; }
    step "Publishing GitHub release v$VERSION"
    gh release create "v$VERSION" "$OUT" "$OUT.sha256" --title "TM-T20III Driver $VERSION" \
        --notes "Signed and notarized installer for macOS 13+ (Apple Silicon and Intel). Download the .pkg and double-click it. SHA-256: $(cut -d' ' -f1 "$OUT.sha256")"
fi

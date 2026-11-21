#!/bin/sh
# Builds a double-clickable macOS installer for the OBS plugin:
#   out/elgato-video-capture-<version>-macos-arm64.pkg
# It installs into the current user's OBS plugin folder and needs no administrator password.
#   tools/package.sh [version]
set -eu
cd "$(dirname "$0")/.."
VERSION="${1:-0.1.0}"
NAME=elgato-video-capture
ARCH="$(uname -m)"
PKG="out/$NAME-$VERSION-macos-$ARCH.pkg"

VERSION="$VERSION" tools/build-obs-plugin.sh

STAGE=out/pkg
rm -rf "$STAGE"
mkdir -p "$STAGE/root" "$STAGE/resources"
cp -R "out/$NAME.plugin" "$STAGE/root/"
xattr -cr "$STAGE/root"

# Always overwrite an existing copy (no version comparison), and never "relocate" the install
# to another copy of the bundle found on disk, such as the one in the build directory.
pkgbuild --analyze --root "$STAGE/root" "$STAGE/component.plist" >/dev/null
/usr/libexec/PlistBuddy -c "Set :0:BundleIsVersionChecked false" \
    -c "Add :0:BundleIsRelocatable bool false" "$STAGE/component.plist" >/dev/null
pkgbuild --quiet --root "$STAGE/root" --component-plist "$STAGE/component.plist" \
    --identifier "local.$NAME.obs-plugin" --version "$VERSION" \
    --install-location "Library/Application Support/obs-studio/plugins" \
    "$STAGE/$NAME.pkg"

cat > "$STAGE/resources/welcome.txt" <<TXT
This installs an OBS Studio source for the Elgato Video Capture USB dongle (model with USB ID 0fd9:0037).

It is installed for your user only, into
~/Library/Application Support/obs-studio/plugins

After installing:
1. Quit Elgato's own Video Capture application if it is open.
2. Restart OBS Studio.
3. Sources > + > Elgato Video Capture (analogue).

Requires OBS Studio 33 on a Mac with Apple silicon, macOS 13 or later.

This is an independent open-source project (GPL-2.0-or-later), not affiliated with Elgato.
TXT

cat > "$STAGE/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Elgato Video Capture for OBS</title>
    <welcome file="welcome.txt"/>
    <options customize="never" require-scripts="false" hostArchitectures="$ARCH"/>
    <domains enable_anywhere="false" enable_currentUserHome="true" enable_localSystem="false"/>
    <os-version min="13.0"/>
    <choices-outline><line choice="default"/></choices-outline>
    <choice id="default" title="OBS plugin"><pkg-ref id="local.$NAME.obs-plugin"/></choice>
    <pkg-ref id="local.$NAME.obs-plugin" version="$VERSION">$NAME.pkg</pkg-ref>
</installer-gui-script>
XML

productbuild --quiet --distribution "$STAGE/distribution.xml" --package-path "$STAGE" \
    --resources "$STAGE/resources" "$PKG"
rm -rf "$STAGE"
echo "packaged $PKG"
shasum -a 256 "$PKG"

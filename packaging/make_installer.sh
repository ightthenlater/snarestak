#!/bin/bash
# Builds SnareStack-<version>-mac.pkg: installs the AU (Logic Pro), the VST3 and the
# standalone app for all users. Run from the repository root after building:
#
#   packaging/make_installer.sh [artefacts dir] [output dir] [version]
#
set -euo pipefail

ART="${1:-build/SnareStack_artefacts/Release}"
OUT="${2:-dist}"
VERSION="${3:-0.1.0}"
HERE="$(cd "$(dirname "$0")" && pwd)"

for f in "$ART/AU/SnareStack.component" "$ART/VST3/SnareStack.vst3"; do
    [ -d "$f" ] || { echo "missing $f (build first)"; exit 1; }
done

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE" "$STAGE.plist"' EXIT
mkdir -p "$STAGE/Library/Audio/Plug-Ins/Components" "$STAGE/Library/Audio/Plug-Ins/VST3" "$STAGE/Applications"
cp -R "$ART/AU/SnareStack.component" "$STAGE/Library/Audio/Plug-Ins/Components/"
cp -R "$ART/VST3/SnareStack.vst3" "$STAGE/Library/Audio/Plug-Ins/VST3/"
[ -d "$ART/Standalone/SnareStack.app" ] && cp -R "$ART/Standalone/SnareStack.app" "$STAGE/Applications/"

# Ad-hoc signature: required for plugins to load on Apple Silicon. (Not notarized:
# macOS will ask you to allow the installer the first time — see the README.)
find "$STAGE" -maxdepth 4 \( -name "*.component" -o -name "*.vst3" -o -name "*.app" \) -print0 |
    while IFS= read -r -d '' b; do codesign --force --deep --sign - "$b"; done

# Always install to the standard folders, never "relocate" onto an old copy elsewhere.
pkgbuild --analyze --root "$STAGE" "$STAGE.plist"
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$STAGE.plist" >/dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$STAGE.plist"
    i=$((i + 1))
done

mkdir -p "$OUT"
pkgbuild --root "$STAGE" \
         --component-plist "$STAGE.plist" \
         --identifier com.snarestack.installer \
         --version "$VERSION" \
         --install-location / \
         --scripts "$HERE/scripts" \
         "$OUT/SnareStack-$VERSION-mac.pkg"

echo "Built $OUT/SnareStack-$VERSION-mac.pkg"

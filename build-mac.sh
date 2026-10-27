#!/bin/bash
# Build SnareStack on your Mac and install it into ~/Library/Audio/Plug-Ins.
#
#   ./build-mac.sh              build + install the AU and VST3 for your user
#   ./build-mac.sh --installer  also make an installer .pkg in ./dist
#
# Needs Apple's command line tools (xcode-select --install) and CMake (brew install cmake).
set -euo pipefail
cd "$(dirname "$0")"

command -v cmake >/dev/null || { echo "CMake not found. Install it with:  brew install cmake"; exit 1; }
xcode-select -p >/dev/null 2>&1 || { echo "Apple command line tools not found. Run:  xcode-select --install"; exit 1; }

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel "$(sysctl -n hw.ncpu)"

# sign the installed copies (needed on Apple Silicon) and refresh the AU cache
for b in "$HOME/Library/Audio/Plug-Ins/Components/SnareStack.component" "$HOME/Library/Audio/Plug-Ins/VST3/SnareStack.vst3"; do
    [ -d "$b" ] && codesign --force --deep --sign - "$b"
done
killall -9 AudioComponentRegistrar 2>/dev/null || true

echo
echo "Checking the Audio Unit the way Logic does..."
if auval -v aufx Sstk Snst >/tmp/snarestack-auval.txt 2>&1; then
    echo "  passed — open Logic Pro and find SnareStack under Audio Units > SnareStack"
else
    echo "  auval reported a problem; details in /tmp/snarestack-auval.txt"
fi

if [ "${1:-}" = "--installer" ]; then
    packaging/make_installer.sh build/SnareStack_artefacts/Release dist
fi

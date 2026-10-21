#!/bin/bash
# Builds dist/Passthrough_PiP-<version>-aarch64.AppImage. Run on a Steam Frame (SteamOS, aarch64): it ships the
# compilers, glslc, mksquashfs and SteamVR's libopenvr_api.so this needs.
#   APPIMAGE_RUNTIME=/path/to/runtime-aarch64  use a local AppImage type-2 runtime (it must match RUNTIME_SHA256)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
DIST="$ROOT/dist"
APPDIR="$BUILD/AppDir"
# a fixed release of the AppImage type-2 runtime, checked against its published SHA-256
RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-aarch64"
RUNTIME_SHA256="00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444"

[ "$(uname -m)" = aarch64 ] || { echo "build on the Steam Frame (aarch64)" >&2; exit 1; }

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(nproc)"

rm -rf "$APPDIR"
cmake --install "$BUILD" --prefix "$APPDIR/usr/lib/framepip" --strip
install -m 755 "$ROOT/packaging/AppRun" "$APPDIR/AppRun"
install -m 644 "$ROOT/packaging/framepip.desktop" "$APPDIR/framepip.desktop"
install -m 644 "$ROOT/resources/icon.png" "$APPDIR/framepip.png"
ln -s framepip.png "$APPDIR/.DirIcon"
VERSION="$(cat "$APPDIR/usr/lib/framepip/share/VERSION")"

RUNTIME="${APPIMAGE_RUNTIME:-$BUILD/runtime-aarch64-20251108}"
if [ ! -s "$RUNTIME" ]; then
  echo "downloading the AppImage runtime"
  curl -fL --retry 3 -o "$RUNTIME" "$RUNTIME_URL"
fi
echo "$RUNTIME_SHA256  $RUNTIME" | sha256sum -c --quiet - || { echo "AppImage runtime checksum mismatch: $RUNTIME" >&2; exit 1; }

rm -f "$BUILD/payload.squashfs"
mksquashfs "$APPDIR" "$BUILD/payload.squashfs" -root-owned -noappend -comp zstd -quiet
mkdir -p "$DIST"
OUT="$DIST/Passthrough_PiP-$VERSION-aarch64.AppImage"
cat "$RUNTIME" "$BUILD/payload.squashfs" > "$OUT"
chmod 755 "$OUT"
echo "built $OUT"

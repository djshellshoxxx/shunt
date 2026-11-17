#!/usr/bin/env bash
# Cross-builds the Windows x64 release on Linux with MinGW-w64 (JUCE 7.0.12),
# runs the unit tests under Wine, and packages a portable zip and an NSIS installer.
# Usage: scripts/build-windows.sh [build-dir] [dist-dir]
# Needs: mingw-w64, ninja, cmake, nsis (makensis), python3 + Pillow, zip; wine for tests.
# Set SHUNT_DEPS_DIR to build offline from pre-cloned dependencies.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build-win}"
DIST="${2:-$ROOT/dist}"
VERSION="$(sed -n 's/^project(Shunt VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
EXTRA=()
[ -n "${SHUNT_DEPS_DIR:-}" ] && EXTRA+=("-DSHUNT_DEPS_DIR=$SHUNT_DEPS_DIR")

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/ToolchainMinGW.cmake" "${EXTRA[@]}"
cmake --build "$BUILD" -j"${JOBS:-$(nproc)}"

if command -v wine >/dev/null && [ "${SKIP_TESTS:-0}" != "1" ]; then
  for t in ShuntTests ShuntOutTests ShuntMetaTests ShuntCoreTests; do
    exe="$BUILD/tests/$t.exe"
    [ -f "$exe" ] || continue
    echo "== $t (wine)"
    (cd "$ROOT/tests" && WINEDEBUG=-all wine "$exe")
  done
fi

STAGE="$DIST/Shunt"
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp "$(find "$BUILD/app/desktop" -name Shunt.exe -path '*Release*' | head -1)" "$STAGE/Shunt.exe"
cp "$(find "$BUILD/app/headless" -name 'shunt_headless.exe' | head -1)" "$STAGE/"
cp "$BUILD/tools/shunt_cli.exe" "$BUILD/tools/shuntsim.exe" "$STAGE/"
python3 - "$ROOT/assets/icon-512.png" "$STAGE/shunt.ico" <<'PY'
import sys
from PIL import Image
Image.open(sys.argv[1]).convert("RGBA").save(sys.argv[2], sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
PY
sed 's/$/\r/' "$ROOT/LEGAL.md" > "$STAGE/LEGAL.txt"
sed 's/$/\r/' "$ROOT/docs/WINDOWS.md" > "$STAGE/README.txt"
x86_64-w64-mingw32-strip "$STAGE"/*.exe

ZIP="$DIST/Shunt-$VERSION-windows-x64-portable.zip"
rm -f "$ZIP" && (cd "$DIST" && zip -qr "$ZIP" Shunt)
makensis -V2 -DVERSION="$VERSION" -DSRCDIR="$STAGE" \
  -DOUTFILE="$DIST/Shunt-$VERSION-windows-x64-setup.exe" "$ROOT/installer/windows/shunt.nsi"
(cd "$DIST" && sha256sum "Shunt-$VERSION"-windows-x64-* > "Shunt-$VERSION-windows-x64.sha256")
ls -la "$DIST"

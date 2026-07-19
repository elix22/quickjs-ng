#!/usr/bin/env bash
# Build libqjs for Windows x64: STATIC (MSVC), all three variants. Runs under the
# GitHub windows runner's bash with the MSVC toolchain on PATH (the workflow calls
# it inside a Developer Command Prompt env). Produces qjs-<variant>.lib.
#
# Part of the threejs-native-runtime prebuilt-libqjs pipeline (phase12 P12.6).
#
#   scripts/prebuilt/build-windows.sh <qjs-src-dir> <out-dir>
#
# Output: <out-dir>/qjs-windows-x64.tar.gz
#   containing lib/qjs-{release,debug,debug-dbg}.lib + include/.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/variants.sh"

SRC="${1:?usage: build-windows.sh <qjs-src-dir> <out-dir>}"
OUT="${2:?usage: build-windows.sh <qjs-src-dir> <out-dir>}"
BUILD="$OUT/build-windows"
STAGE="$OUT/stage-windows"
rm -rf "$BUILD" "$STAGE"

# MSVC is a multi-config generator: CMAKE_BUILD_TYPE at configure is ignored, so
# variant_cmake_args' build-type is passed AGAIN at build time via --config. The
# debugger define still rides CMAKE_C_FLAGS (cl accepts -D).
build_win_variant() {
  local variant="$1" config extra
  case "$variant" in
    release)   config=Release; extra="" ;;
    debug)     config=Debug;   extra="" ;;
    debug-dbg) config=Debug;   extra="-DCMAKE_C_FLAGS=-DTNR_QJS_DEBUGGER" ;;
  esac
  local dir="$BUILD/$variant"
  rm -rf "$dir"
  cmake -S "$SRC" -B "$dir" -A x64 \
    -DQJS_BUILD_EXAMPLES=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    $extra
  cmake --build "$dir" --target qjs --config "$config" --parallel
  local built
  built=$(find "$dir" -name 'qjs.lib' | head -1)
  [ -n "$built" ] || { echo "FAIL: $variant produced no qjs.lib"; exit 1; }
  mkdir -p "$STAGE/lib"
  cp "$built" "$STAGE/lib/qjs-$variant.lib"
  echo "  [$variant] -> $STAGE/lib/qjs-$variant.lib"
}

for v in "${QJS_VARIANTS[@]}"; do build_win_variant "$v"; done
stage_headers "$SRC" "$STAGE/include"

# dumpbin is the MSVC symbol tool; grep the debugger entry point in/out of variants.
DBG=$(dumpbin //symbols "$STAGE/lib/qjs-debug-dbg.lib" 2>/dev/null | grep -c 'js_debugger_attach' || true)
[ "$DBG" -ge 1 ] || { echo "FAIL: debug-dbg lacks js_debugger_attach"; exit 1; }
REL=$(dumpbin //symbols "$STAGE/lib/qjs-release.lib" 2>/dev/null | grep -c 'js_debugger_attach' || true)
[ "$REL" -eq 0 ] || { echo "FAIL: release variant leaked the debugger engine"; exit 1; }

{ echo "source-commit: $(git -C "$SRC" rev-parse HEAD)"
  echo "config: static x64 (MSVC); variants release(-O2) debug(-O0) debug-dbg(-O0 +TNR_QJS_DEBUGGER)"
} > "$STAGE/MANIFEST.txt"

tar -C "$STAGE" -czf "$OUT/qjs-windows-x64.tar.gz" .
echo "artifact: $OUT/qjs-windows-x64.tar.gz"

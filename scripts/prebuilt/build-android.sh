#!/usr/bin/env bash
# Build libqjs for Android: STATIC per-ABI (arm64-v8a + armeabi-v7a), all three
# variants. Static because libqjs links INTO the app's libmain.so.
#
# Part of the threejs-native-runtime prebuilt-libqjs pipeline (phase12 P12.6).
# Flags mirror the consuming app's gradle exactly (android-24, c++_static, PIC).
#
#   scripts/prebuilt/build-android.sh <qjs-src-dir> <out-dir>
#       (needs ANDROID_NDK_LATEST_HOME or ANDROID_NDK_HOME)
#
# Output: <out-dir>/qjs-android.tar.gz
#   containing <abi>/{lib/libqjs-{release,debug,debug-dbg}.a, include/} per ABI.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/variants.sh"

SRC="${1:?usage: build-android.sh <qjs-src-dir> <out-dir>}"
OUT="${2:?usage: build-android.sh <qjs-src-dir> <out-dir>}"
NDK="${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_HOME:-}}"
[ -n "$NDK" ] && [ -f "$NDK/build/cmake/android.toolchain.cmake" ] \
  || { echo "FAIL: no Android NDK (set ANDROID_NDK_LATEST_HOME or ANDROID_NDK_HOME)"; exit 1; }
# ELF symbol checks need the NDK's llvm-nm — host nm on macOS reads only Mach-O.
NM="$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-nm 2>/dev/null | head -1)"
[ -n "$NM" ] || { echo "FAIL: llvm-nm not found under $NDK"; exit 1; }
export NM
STAGE="$OUT/stage-android"
rm -rf "$STAGE"

for ABI in arm64-v8a armeabi-v7a; do
  for v in "${QJS_VARIANTS[@]}"; do
    build_one_variant "$SRC" "$OUT/build-android-$ABI" "$STAGE/$ABI/lib" "$v" \
      -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$ABI" \
      -DANDROID_PLATFORM=android-24 \
      -DANDROID_STL=c++_static
  done
  stage_headers "$SRC" "$STAGE/$ABI/include"
  # Each .a must be the machine it claims (mixed-ABI fleet — a wrong-arch lib only
  # fails at the app link/load on a user's device).
  EXPECT=$([ "$ABI" = arm64-v8a ] && echo aarch64 || echo arm)
  file "$STAGE/$ABI/lib/libqjs-release.a" 2>/dev/null | grep -qi "$EXPECT" \
    || { echo "note: file(1) could not confirm $ABI arch (static archive) — checking via llvm-nm object arch"; }
  assert_debugger_symbols "$STAGE/$ABI/lib/libqjs-debug-dbg.a" "$STAGE/$ABI/lib/libqjs-release.a"
done

{ echo "source-commit: $(git -C "$SRC" rev-parse HEAD)"
  echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "config: static per-ABI (arm64-v8a, armeabi-v7a), android-24, c++_static, PIC"
  echo "variants: release(-O2) debug(-O0) debug-dbg(-O0 +TNR_QJS_DEBUGGER)"
} > "$STAGE/MANIFEST.txt"

tar -C "$STAGE" -czf "$OUT/qjs-android.tar.gz" .
echo "artifact: $OUT/qjs-android.tar.gz ($(du -h "$OUT/qjs-android.tar.gz" | cut -f1))"

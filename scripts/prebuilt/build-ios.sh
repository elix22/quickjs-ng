#!/usr/bin/env bash
# Build libqjs for iOS: STATIC, device (arm64) + simulator (arm64 + x86_64), all
# three variants. Flat per-slice trees (lib/ + include/) — NOT an xcframework:
# the runtime's CMake imports libqjs by slice path (libs/qjs/ios/{device,
# simulator}), mirroring how it consumes the fetched SDL slices.
#
# Part of the threejs-native-runtime prebuilt-libqjs pipeline (phase12 P12.6).
#
#   scripts/prebuilt/build-ios.sh <qjs-src-dir> <out-dir>
#
# Output: <out-dir>/qjs-ios-static.tar.gz
#   containing {device,simulator}/{lib/libqjs-{release,debug,debug-dbg}.a, include}.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/variants.sh"

SRC="${1:?usage: build-ios.sh <qjs-src-dir> <out-dir>}"
OUT="${2:?usage: build-ios.sh <qjs-src-dir> <out-dir>}"
STAGE="$OUT/stage-ios"
rm -rf "$STAGE"

# build_slice <slice-name> <sysroot> <archs>
build_slice() {
  local slice="$1" sysroot="$2" archs="$3" v
  for v in "${QJS_VARIANTS[@]}"; do
    build_one_variant "$SRC" "$OUT/build-ios-$slice" "$STAGE/$slice/lib" "$v" \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_SYSROOT="$sysroot" \
      -DCMAKE_OSX_ARCHITECTURES="$archs" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
  done
  stage_headers "$SRC" "$STAGE/$slice/include"
  # Each slice must carry exactly the archs it claims (a mismatched simulator
  # slice only fails much later on someone's Intel Mac).
  local got
  got=$(lipo -archs "$STAGE/$slice/lib/libqjs-release.a")
  echo "$slice archs: $got"
  case "$archs" in *x86_64*) case "$got" in *x86_64*) ;; *) echo "FAIL: $slice x86_64 missing"; exit 1 ;; esac ;; esac
  case "$got" in *arm64*) ;; *) echo "FAIL: $slice arm64 missing"; exit 1 ;; esac
  assert_debugger_symbols "$STAGE/$slice/lib/libqjs-debug-dbg.a" "$STAGE/$slice/lib/libqjs-release.a"
}

build_slice device    iphoneos        "arm64"
build_slice simulator iphonesimulator "arm64;x86_64"

{ echo "source-commit: $(git -C "$SRC" rev-parse HEAD)"
  echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "config: static iOS device(arm64) + simulator(arm64;x86_64), min iOS 13.0"
  echo "variants: release(-O2) debug(-O0) debug-dbg(-O0 +TNR_QJS_DEBUGGER)"
} > "$STAGE/MANIFEST.txt"

tar -C "$STAGE" -czf "$OUT/qjs-ios-static.tar.gz" .
echo "artifact: $OUT/qjs-ios-static.tar.gz ($(du -h "$OUT/qjs-ios-static.tar.gz" | cut -f1))"

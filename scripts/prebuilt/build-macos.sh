#!/usr/bin/env bash
# Build libqjs for macOS: STATIC, UNIVERSAL (arm64 + x86_64), all three variants.
#
# Part of the threejs-native-runtime prebuilt-libqjs pipeline (phase12 P12.6): CI
# runs this and publishes the result as a GitHub Release asset; the runtime's
# fetch-libs step downloads it into libs/qjs/macos/. Users can also run it locally
# (tools/build-libs.mjs quickjs) — identical layout, drop-in over the fetched one.
#
#   scripts/prebuilt/build-macos.sh <qjs-src-dir> <out-dir>
#
# Output: <out-dir>/qjs-macos-universal.tar.gz
#   containing lib/libqjs-{release,debug,debug-dbg}.a (each universal) + include/.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/variants.sh"

SRC="${1:?usage: build-macos.sh <qjs-src-dir> <out-dir>}"
OUT="${2:?usage: build-macos.sh <qjs-src-dir> <out-dir>}"
BUILD="$OUT/build-macos"
STAGE="$OUT/stage-macos"
rm -rf "$BUILD" "$STAGE"

for v in "${QJS_VARIANTS[@]}"; do
  build_one_variant "$SRC" "$BUILD" "$STAGE/lib" "$v" \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
done
stage_headers "$SRC" "$STAGE/include"

# Each variant must actually be static and carry BOTH arches (a single-arch lib
# only fails much later as "cannot link for x86_64" on an Intel Mac — fail here).
for v in "${QJS_VARIANTS[@]}"; do
  LIB="$STAGE/lib/libqjs-$v.a"
  [ -f "$LIB" ] || { echo "FAIL: $LIB missing"; exit 1; }
  ARCHS=$(lipo -archs "$LIB")
  echo "libqjs-$v.a archs: $ARCHS"
  case "$ARCHS" in *arm64*) ;; *) echo "FAIL: $v arm64 slice missing"; exit 1 ;; esac
  case "$ARCHS" in *x86_64*) ;; *) echo "FAIL: $v x86_64 slice missing"; exit 1 ;; esac
done
assert_debugger_symbols "$STAGE/lib/libqjs-debug-dbg.a" "$STAGE/lib/libqjs-release.a"

{ echo "source-commit: $(git -C "$SRC" rev-parse HEAD)"
  echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "config: static universal (arm64;x86_64); variants release(-O2) debug(-O0) debug-dbg(-O0 +TNR_QJS_DEBUGGER)"
} > "$STAGE/MANIFEST.txt"

tar -C "$STAGE" -czf "$OUT/qjs-macos-universal.tar.gz" .
echo "artifact: $OUT/qjs-macos-universal.tar.gz ($(du -h "$OUT/qjs-macos-universal.tar.gz" | cut -f1))"

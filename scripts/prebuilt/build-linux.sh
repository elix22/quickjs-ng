#!/usr/bin/env bash
# Build libqjs for Linux x86_64: STATIC, all three variants.
#
# Part of the threejs-native-runtime prebuilt-libqjs pipeline (phase12 P12.6).
#
#   scripts/prebuilt/build-linux.sh <qjs-src-dir> <out-dir>
#
# Output: <out-dir>/qjs-linux-x64.tar.gz
#   containing lib/libqjs-{release,debug,debug-dbg}.a + include/.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/variants.sh"

SRC="${1:?usage: build-linux.sh <qjs-src-dir> <out-dir>}"
OUT="${2:?usage: build-linux.sh <qjs-src-dir> <out-dir>}"
BUILD="$OUT/build-linux"
STAGE="$OUT/stage-linux"
rm -rf "$BUILD" "$STAGE"

for v in "${QJS_VARIANTS[@]}"; do
  build_one_variant "$SRC" "$BUILD" "$STAGE/lib" "$v"
done
stage_headers "$SRC" "$STAGE/include"

for v in "${QJS_VARIANTS[@]}"; do
  file "$STAGE/lib/libqjs-$v.a" | grep -qi "archive" || { echo "FAIL: libqjs-$v.a not an archive"; exit 1; }
done
assert_debugger_symbols "$STAGE/lib/libqjs-debug-dbg.a" "$STAGE/lib/libqjs-release.a"

{ echo "source-commit: $(git -C "$SRC" rev-parse HEAD)"
  echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "config: static x86_64; variants release(-O2) debug(-O0) debug-dbg(-O0 +TNR_QJS_DEBUGGER)"
} > "$STAGE/MANIFEST.txt"

tar -C "$STAGE" -czf "$OUT/qjs-linux-x64.tar.gz" .
echo "artifact: $OUT/qjs-linux-x64.tar.gz ($(du -h "$OUT/qjs-linux-x64.tar.gz" | cut -f1))"

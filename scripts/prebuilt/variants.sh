#!/usr/bin/env bash
# Shared variant recipe for the threejs-native-runtime prebuilt-libqjs pipeline
# (phase12 P12.6). Sourced by every per-platform build-*.sh below.
#
# Three published variants, named by the STATIC-LIB convention so the consuming
# CMake resolves them with CMAKE_STATIC_LIBRARY_PREFIX/SUFFIX on any platform:
#
#   release     libqjs-release.a     -O2, interpreter only
#   debug       libqjs-debug.a       -O0, interpreter only
#   debug-dbg   libqjs-debug-dbg.a   -O0 + TNR_QJS_DEBUGGER (the DAP engine)
#
# The debugger define is NOT a submodule CMake option — the runtime's root
# CMakeLists adds it to the `qjs` target after add_subdirectory(). Standalone we
# inject it through CMAKE_C_FLAGS, which makes quickjs.c pull in quickjs-debugger.c
# at its tail exactly as the from-source debug host does.
#
# AOT twins are never baked here: quickjs.c is compiled with no
# TNR_AOT_GENERATED_C, so the AOT machinery is present but registers zero twins
# (the hard constraint — app twins must compile inside quickjs.c's own TU at the
# runtime's build time, so a prebuilt can never carry them).
#
# PIC everywhere: on Android the static libqjs links into libmain.so.
#
# Header set shipped alongside the libs = the runtime host's transitive include
# closure (src/host includes quickjs.h, quickjs-debugger.h, quickjs-aot.h; the
# last pulls quickjs-opcode.h). The debugger header ships in every variant; only
# the consumer's -DTNR_QJS_DEBUGGER differs.
set -euo pipefail

QJS_VARIANTS=(release debug debug-dbg)
QJS_HEADERS=(quickjs.h quickjs-debugger.h quickjs-aot.h quickjs-opcode.h)

# variant_cmake_args <variant> -> echoes the extra -D flags for that variant.
variant_cmake_args() {
  case "$1" in
    release)   echo "-DCMAKE_BUILD_TYPE=Release" ;;
    debug)     echo "-DCMAKE_BUILD_TYPE=Debug" ;;
    debug-dbg) echo "-DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS=-DTNR_QJS_DEBUGGER" ;;
    *) echo "unknown variant: $1" >&2; return 1 ;;
  esac
}

# assert_debugger_symbols <debug-dbg-lib> <release-lib>
# The debugger variant MUST carry the engine and the release variant MUST NOT.
# grep -c (not grep -q): grep -q exits on the first match and SIGPIPEs nm, which
# under `set -o pipefail` fails the pipeline on a SUCCESSFUL match. Count to EOF.
assert_debugger_symbols() {
  local dbg="$1" rel="$2" n nm="${NM:-nm}"   # NM overridable (Android needs llvm-nm for ELF)
  n=$("$nm" "$dbg" 2>/dev/null | grep -c 'js_debugger_attach' || true)
  [ "$n" -ge 1 ] || { echo "FAIL: $(basename "$dbg") lacks js_debugger_attach"; return 1; }
  n=$("$nm" "$rel" 2>/dev/null | grep -c 'js_debugger_attach' || true)
  [ "$n" -eq 0 ] || { echo "FAIL: $(basename "$rel") leaked the debugger engine"; return 1; }
}

# stage_headers <src-dir> <dest-include-dir>
stage_headers() {
  local src="$1" dst="$2"
  mkdir -p "$dst"
  local h
  for h in "${QJS_HEADERS[@]}"; do
    [ -f "$src/$h" ] || { echo "FAIL: header $src/$h missing"; return 1; }
    cp "$src/$h" "$dst/$h"
  done
}

# build_one_variant <src-dir> <build-root> <install-lib-dir> <variant> [extra cmake args...]
# Configures the submodule's OWN CMake (source of truth for qjs_sources + defines),
# builds ONLY the `qjs` static target, and copies libqjs-<variant>.<ext> into the
# install lib dir under its variant name.
build_one_variant() {
  local src="$1" buildroot="$2" libdir="$3" variant="$4"; shift 4
  local build="$buildroot/$variant"
  rm -rf "$build"
  # shellcheck disable=SC2046
  cmake -S "$src" -B "$build" \
    -DQJS_BUILD_EXAMPLES=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    $(variant_cmake_args "$variant") \
    "$@"
  cmake --build "$build" --target qjs --parallel
  # The static target `qjs` emits <prefix>qjs<suffix> (libqjs.a / qjs.lib). Find it
  # rather than guessing the generator's output dir.
  local built
  built=$(find "$build" -name 'libqjs.a' -o -name 'qjs.lib' | head -1)
  [ -n "$built" ] || { echo "FAIL: $variant produced no static libqjs"; return 1; }
  mkdir -p "$libdir"
  # Preserve the platform's static-lib prefix/ext so the consuming CMake resolves
  # the file with CMAKE_STATIC_LIBRARY_PREFIX/SUFFIX: unix libqjs.a -> libqjs-<v>.a,
  # MSVC qjs.lib -> qjs-<v>.lib.
  local base ext prefix out
  base=$(basename "$built"); ext="${base##*.}"; prefix="${base%.*}"; prefix="${prefix%qjs}"
  out="$libdir/${prefix}qjs-${variant}.${ext}"
  cp "$built" "$out"
  # RELEASE ships no debug info. The Android NDK adds -g to EVERY config (it expects
  # the final .so to be stripped by Gradle), so an un-stripped -O3 release archive
  # carries huge optimized-code DWARF — bigger than the -O0 debug archive. Strip it
  # so `release` is genuinely the lean variant. `--strip-debug` keeps .symtab (link
  # symbols) and DWARF-only; hosts whose strip lacks the flag (macOS) have no DWARF
  # in release anyway, so the no-op is correct.
  if [ "$variant" = release ]; then
    "${STRIP:-strip}" --strip-debug "$out" 2>/dev/null || true
  fi
  echo "  [$variant] -> $out ($(du -h "$out" | cut -f1))"
}

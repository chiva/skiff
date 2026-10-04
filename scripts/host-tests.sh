#!/usr/bin/env bash
# Configures, builds and runs one host test preset with one compiler, in its own build tree so the
# gcc and clang builds never share a CMake cache. Runs inside the host image (scripts/dev.sh).
# Fails if CMake picks a compiler other than the one requested, so a toolchain change cannot
# silently turn the clang run into a second gcc run.
# Usage: scripts/host-tests.sh <preset> <gcc|clang>
set -euo pipefail

readonly USAGE="usage: scripts/host-tests.sh <preset> <gcc|clang>"
readonly PRESET="${1:?$USAGE}"
readonly COMPILER="${2:?$USAGE}"
readonly BUILD_DIR="build/$PRESET-$COMPILER"

case "$COMPILER" in
gcc) expected_id="GNU" ;;
clang) expected_id="Clang" ;;
*)
  echo "$USAGE" >&2
  exit 2
  ;;
esac

echo "== $PRESET ($COMPILER) -> $BUILD_DIR"
CC="$COMPILER" cmake --preset "$PRESET" -B "$BUILD_DIR" >/dev/null
# The configure log names the compiler only on a fresh tree; the recorded ID survives re-runs.
if ! grep -q "set(CMAKE_C_COMPILER_ID \"$expected_id\")" "$BUILD_DIR"/CMakeFiles/*/CMakeCCompiler.cmake; then
  echo "error: requested $COMPILER but CMake picked another compiler" >&2
  exit 1
fi
cmake --build "$BUILD_DIR"
# Not `ctest --preset`: the test preset's binaryDir overrides --test-dir and would run another tree.
ctest --test-dir "$BUILD_DIR" --output-on-failure --verbose

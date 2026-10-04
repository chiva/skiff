#!/usr/bin/env bash
# One entry point for every local build and check. Everything runs in containers, so Docker is the
# only prerequisite and results match CI. Run `scripts/dev.sh help` for the command list.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT
readonly TOOLCHAIN_IMAGE="skiff-toolchain"
readonly HOST_IMAGE="skiff-host"
readonly PPSSPP_IMAGE="skiff-ppsspp"
readonly COVERAGE_FLOOR=85
# CI runs `test` and `asan` through this script, so this list is the compiler matrix everywhere.
readonly HOST_COMPILERS=(gcc clang)

usage() {
  cat <<'EOF'
Usage: scripts/dev.sh <command> [<command>...]

Commands run in the order given and stop at the first failure.

  psp          Build the debug EBOOTs        -> build/psp/pbp/
  psp-release  Build the release EBOOTs      -> build/psp-release/pbp/
  package      Build release and zip it      -> dist/skiff-<version>.zip
  test         Host unit tests, gcc and clang  -> build/host-{gcc,clang}/
  asan         Host unit tests under ASan + UBSan, gcc and clang
  coverage     Host unit tests with coverage (fails under 85% line coverage)
  lint         clang-tidy and cppcheck over first-party sources
  selftest     Run the self-test EBOOT in PPSSPPHeadless (needs `psp` first)
  tls-probe    Run the TLS toolchain probe in PPSSPPHeadless (needs `psp` first)
  clean        Remove build/ and dist/
EOF
}

ensure_toolchain_image() {
  # pspdev publishes amd64 images only; Apple Silicon runs them under emulation. The first build
  # compiles mbedtls and curl and takes minutes there; later runs are cached until the Dockerfile
  # changes.
  echo "toolchain image: building if docker/toolchain.Dockerfile changed..." >&2
  docker build --quiet --platform linux/amd64 -t "$TOOLCHAIN_IMAGE" \
    -f "$REPO_ROOT/docker/toolchain.Dockerfile" "$REPO_ROOT/docker" >/dev/null
}

run_toolchain() {
  ensure_toolchain_image
  docker run --rm --platform linux/amd64 -v "$REPO_ROOT":/src -w /src "$TOOLCHAIN_IMAGE" bash -c "$1"
}

run_emulator() {
  local eboot="$1" name="$2"
  ensure_ppsspp_image
  docker run --rm -v "$REPO_ROOT":/src -w /src "$PPSSPP_IMAGE" tests/emulator/run_eboot.sh "$eboot" "$name"
}

ensure_host_image() {
  docker build --quiet -t "$HOST_IMAGE" -f "$REPO_ROOT/docker/host.Dockerfile" "$REPO_ROOT/docker" >/dev/null
}

ensure_ppsspp_image() {
  docker build --quiet -t "$PPSSPP_IMAGE" -f "$REPO_ROOT/docker/ppsspp.Dockerfile" "$REPO_ROOT/docker" >/dev/null
}

run_host() {
  ensure_host_image
  docker run --rm -v "$REPO_ROOT":/src -w /src "$HOST_IMAGE" bash -c "$1"
}

host_tests() {
  local preset="$1" compiler
  for compiler in "${HOST_COMPILERS[@]}"; do
    run_host "scripts/host-tests.sh $preset $compiler"
  done
}

run_command() {
  local cmd="$1"
  case "$cmd" in
  psp | psp-release)
    run_toolchain "cmake --preset $cmd >/dev/null && cmake --build --preset $cmd"
    ;;
  package)
    run_toolchain "cmake --preset psp-release >/dev/null && cmake --build --preset psp-release \
      && scripts/collect-licenses.sh build/psp-release/licenses >/dev/null"
    "$REPO_ROOT/scripts/package.sh" "$REPO_ROOT/build/psp-release/pbp/skiff/EBOOT.PBP" \
      "$REPO_ROOT/build/psp-release/licenses/third-party-licenses"
    ;;
  test)
    host_tests host
    ;;
  asan)
    host_tests host-asan
    ;;
  coverage)
    run_host "cmake --preset host-coverage >/dev/null && cmake --build --preset host-coverage \
      && ctest --preset host-coverage"
    run_host "gcovr --root . --filter src/ --exclude src/platform/ --fail-under-line $COVERAGE_FLOOR \
      --print-summary --txt --xml build/host-coverage/coverage.xml"
    ;;
  lint)
    run_host "cmake --preset host >/dev/null && scripts/lint.sh build/host"
    ;;
  selftest)
    run_emulator build/psp/pbp/skiff_selftest/EBOOT.PBP SELFTEST
    ;;
  tls-probe)
    run_emulator build/psp/pbp/skiff_tls_probe/EBOOT.PBP "TLS PROBE"
    ;;
  clean)
    rm -rf "$REPO_ROOT/build" "$REPO_ROOT/dist"
    ;;
  help | -h | --help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
  esac
}

if [[ $# -eq 0 ]]; then
  usage
  exit 0
fi
for cmd in "$@"; do
  run_command "$cmd"
done

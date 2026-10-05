#!/usr/bin/env bash
# Static analysis over first-party portable sources. Needs a configured host build tree for
# compile_commands.json. Lints exactly the tracked sources the host build compiles, so PSP-only
# files (which host clang cannot build without the PSP SDK) are excluded automatically, wherever
# they live.
# Usage: scripts/lint.sh <build-dir>
set -euo pipefail

readonly BUILD_DIR="${1:?usage: scripts/lint.sh <build-dir>}"
readonly COMPILE_COMMANDS="$BUILD_DIR/compile_commands.json"

# Command substitutions rather than process substitutions, so a failing git or jq aborts under
# set -e instead of silently yielding an empty list.
tracked="$(git ls-files 'src/*.c' 'tests/*.c')"
compiled="$(jq -r '.[].file' "$COMPILE_COMMANDS")"

sources=()
while IFS= read -r file; do
  if grep -qxF "$PWD/$file" <<<"$compiled"; then
    sources+=("$file")
  fi
done <<<"$tracked"

if [[ ${#sources[@]} -eq 0 ]]; then
  echo "error: no first-party C sources found to lint" >&2
  exit 1
fi

echo "clang-tidy: ${#sources[@]} files"
clang-tidy --quiet -p "$BUILD_DIR" "${sources[@]}"

echo "cppcheck"
cppcheck --quiet --error-exitcode=1 --enable=warning,style,performance,portability --inline-suppr \
  --suppress=missingIncludeSystem --std=c11 -I include "${sources[@]}"

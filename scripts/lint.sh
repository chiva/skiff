#!/usr/bin/env bash
# Static analysis over first-party portable sources. Needs a configured host build tree for
# compile_commands.json. PSP-only sources are skipped because host clang cannot see the PSP SDK or
# the toolchain image's TLS stack; they are identified by including a PSP SDK header (<psp...>) or
# an Mbed TLS one (<mbedtls/...>, <psa/...>) rather than by a hard-coded path, so a new PSP-only
# file anywhere is excluded automatically.
# Usage: scripts/lint.sh <build-dir>
set -euo pipefail

readonly BUILD_DIR="${1:?usage: scripts/lint.sh <build-dir>}"

# A command substitution rather than a process substitution, so a failing git aborts under set -e
# instead of silently yielding an empty list.
tracked="$(git ls-files 'src/*.c' 'tests/*.c')"

sources=()
while IFS= read -r file; do
  if grep -qE '^\s*#\s*include\s*<(psp|mbedtls/|psa/)' "$file"; then
    continue
  fi
  sources+=("$file")
done <<<"$tracked"

if [[ ${#sources[@]} -eq 0 || -z "${sources[0]}" ]]; then
  echo "error: no first-party C sources found to lint" >&2
  exit 1
fi

echo "clang-tidy: ${#sources[@]} files"
clang-tidy --quiet -p "$BUILD_DIR" "${sources[@]}"

echo "cppcheck"
cppcheck --quiet --error-exitcode=1 --enable=warning,style,performance,portability --inline-suppr \
  --suppress=missingIncludeSystem --std=c11 -I include "${sources[@]}"

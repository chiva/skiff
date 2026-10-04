#!/usr/bin/env bash
# Static analysis over first-party portable sources. Needs a configured host build tree for
# compile_commands.json. PSP-only sources are skipped because host clang cannot see the PSP SDK;
# they are identified by including a PSP SDK header (<psp...>) rather than by a hard-coded path, so
# a new PSP-only file anywhere is excluded automatically.
# Usage: scripts/lint.sh <build-dir>
set -euo pipefail

readonly BUILD_DIR="${1:?usage: scripts/lint.sh <build-dir>}"

sources=()
while IFS= read -r file; do
  if grep -qE '^\s*#\s*include\s*<psp' "$file"; then
    continue
  fi
  sources+=("$file")
done < <(git ls-files 'src/*.c' 'tests/*.c')

echo "clang-tidy: ${#sources[@]} files"
clang-tidy --quiet -p "$BUILD_DIR" "${sources[@]}"

echo "cppcheck"
cppcheck --quiet --error-exitcode=1 --enable=warning,style,performance,portability --inline-suppr \
  --suppress=missingIncludeSystem --std=c11 -I include "${sources[@]}"

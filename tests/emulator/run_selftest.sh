#!/usr/bin/env bash
# Boots the self-test EBOOT in PPSSPPHeadless and passes only if the on-device checks report OK.
# The marker strings must match SKIFF_SELFTEST_OK_MARKER / SKIFF_SELFTEST_FAIL_MARKER in
# include/skiff/selftest.h.
# Usage: tests/emulator/run_selftest.sh <path/to/selftest/EBOOT.PBP>
set -euo pipefail

readonly EBOOT="${1:?usage: run_selftest.sh <path/to/EBOOT.PBP>}"
readonly TIMEOUT_SECONDS=30
readonly OK_MARKER="SKIFF SELFTEST OK"
readonly FAIL_MARKER="SKIFF SELFTEST FAIL"
# PPSSPPHeadless only surfaces the program's stdout inside its full log (-l), one line per write,
# as "<level> stdout: <text>". The full log is kept for diagnosis when the marker is missing.
readonly STDOUT_PREFIX_PATTERN='^[A-Z] stdout: '

if [[ ! -f "$EBOOT" ]]; then
  echo "error: $EBOOT not found; build it first with scripts/dev.sh psp" >&2
  exit 1
fi

log="$(PPSSPPHeadless -l --graphics=software --timeout="$TIMEOUT_SECONDS" "$EBOOT" 2>&1 || true)"
output="$(grep -E "$STDOUT_PREFIX_PATTERN" <<<"$log" | sed -E "s/$STDOUT_PREFIX_PATTERN//" || true)"
printf '%s\n' "$output"

if grep -q "^$FAIL_MARKER" <<<"$output"; then
  echo "emulator self-test: checks failed" >&2
  exit 1
fi
if ! grep -q "^$OK_MARKER" <<<"$output"; then
  echo "emulator self-test: no result marker; the EBOOT crashed or timed out. Last log lines:" >&2
  tail -n 40 <<<"$log" >&2
  exit 1
fi
echo "emulator self-test: OK"

#!/usr/bin/env bash
# Boots a check EBOOT in PPSSPPHeadless and passes only if it reports "SKIFF <NAME> OK". Used for
# the self-test (NAME=SELFTEST, markers in include/skiff/selftest.h) and the TLS toolchain probe
# (NAME="TLS PROBE", markers in tests/security/tls_probe.c).
# Usage: tests/emulator/run_eboot.sh <path/to/EBOOT.PBP> <NAME>
set -euo pipefail

readonly USAGE="usage: run_eboot.sh <path/to/EBOOT.PBP> <NAME>"
readonly EBOOT="${1:?$USAGE}"
readonly NAME="${2:?$USAGE}"
readonly TIMEOUT_SECONDS=30
readonly OK_MARKER="SKIFF $NAME OK"
readonly FAIL_MARKER="SKIFF $NAME FAIL"
# PPSSPPHeadless only surfaces the program's stdout inside its full log (-l), one line per write,
# as "<level> stdout: <text>". The full log is kept for diagnosis when the marker is missing.
readonly STDOUT_PREFIX_PATTERN='^[A-Z] stdout: '
# PPSSPP logs this before any of the EBOOT's code runs. PPSSPPHeadless occasionally dies during its
# own kernel start-up (seen on amd64 CI runners); a run without this line never reached our code.
readonly KERNEL_READY_LINE='Kernel initialized.'
readonly MAX_ATTEMPTS=2

if [[ ! -f "$EBOOT" ]]; then
  echo "error: $EBOOT not found; build it first with scripts/dev.sh psp" >&2
  exit 1
fi

# Retries only an emulator that died before loading the EBOOT. An EBOOT that started and then
# crashed or reported FAIL is never retried, so a flaky bug in our code cannot hide behind this.
for attempt in $(seq 1 "$MAX_ATTEMPTS"); do
  status=0
  log="$(PPSSPPHeadless -l --graphics=software --timeout="$TIMEOUT_SECONDS" "$EBOOT" 2>&1)" || status=$?
  if grep -qF "$KERNEL_READY_LINE" <<<"$log"; then
    break
  fi
  echo "emulator $NAME: PPSSPPHeadless exited ($status) before its kernel started, attempt" \
    "$attempt of $MAX_ATTEMPTS; the EBOOT never ran" >&2
done
output="$(grep -E "$STDOUT_PREFIX_PATTERN" <<<"$log" | sed -E "s/$STDOUT_PREFIX_PATTERN//" || true)"
printf '%s\n' "$output"

if grep -q "^$FAIL_MARKER" <<<"$output"; then
  echo "emulator $NAME: checks failed" >&2
  exit 1
fi
if ! grep -q "^$OK_MARKER" <<<"$output"; then
  echo "emulator $NAME: no result marker (PPSSPPHeadless exit $status); the EBOOT crashed or" \
    "timed out. Last log lines:" >&2
  tail -n 40 <<<"$log" >&2
  exit 1
fi
echo "emulator $NAME: OK"

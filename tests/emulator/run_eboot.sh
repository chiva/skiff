#!/usr/bin/env bash
# Boots a check EBOOT (or the launch check's disc image) in PPSSPPHeadless and passes only if it
# reports "SKIFF <NAME> OK". Used for the self-test (NAME=SELFTEST, markers in
# include/skiff/selftest.h) and the TLS toolchain probe (NAME="TLS PROBE", markers in
# tests/security/tls_probe.c).
# Usage: tests/emulator/run_eboot.sh <path/to/EBOOT.PBP or .iso/.cso> <NAME> [timeout seconds]
#
# The timeout is real time, while an EBOOT paces itself in emulated time, and the software renderer
# draws every frame on the host's CPU: an EBOOT that draws for a fixed emulated time takes longer on
# a slow host. The UI prototype draws a full screen for 10 emulated seconds, which took 8 to 116 s
# on one Mac and ran past 30 s on CI runners, so it passes a longer timeout.
set -euo pipefail

readonly USAGE="usage: run_eboot.sh <path/to/EBOOT.PBP> <NAME> [timeout seconds]"
readonly EBOOT="${1:?$USAGE}"
readonly NAME="${2:?$USAGE}"
readonly DEFAULT_TIMEOUT_SECONDS=30
readonly TIMEOUT_SECONDS="${3:-$DEFAULT_TIMEOUT_SECONDS}"
readonly OK_MARKER="SKIFF $NAME OK"
readonly FAIL_MARKER="SKIFF $NAME FAIL"
# PPSSPPHeadless only surfaces the program's stdout inside its full log (-l), one line per write,
# as "<level> stdout: <text>". The full log is kept for diagnosis when the marker is missing.
readonly STDOUT_PREFIX_PATTERN='^[A-Z] stdout: '
# PPSSPP logs this before any of the EBOOT's code runs. PPSSPPHeadless occasionally dies during its
# own kernel start-up (seen on amd64 CI runners); a run without this line never reached our code.
readonly KERNEL_READY_LINE='Kernel initialized.'
readonly MAX_ATTEMPTS=2

if [[ ! "$TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: timeout must be a positive number of seconds, got '$TIMEOUT_SECONDS'" >&2
  exit 1
fi
if [[ ! -f "$EBOOT" ]]; then
  echo "error: $EBOOT not found; build it first with scripts/dev.sh psp" >&2
  exit 1
fi

# Retries only an emulator that died before loading the EBOOT: no kernel line AND no EBOOT output at
# all. An EBOOT that printed anything, crashed or reported FAIL is never retried, so a flaky bug in
# our code cannot hide behind this, even if a PPSSPP update renames or drops the kernel line.
for attempt in $(seq 1 "$MAX_ATTEMPTS"); do
  status=0
  started=$SECONDS
  log="$(PPSSPPHeadless -l --graphics=software --timeout="$TIMEOUT_SECONDS" "$EBOOT" 2>&1)" || status=$?
  elapsed=$((SECONDS - started))
  if grep -qF "$KERNEL_READY_LINE" <<<"$log" || grep -qE "$STDOUT_PREFIX_PATTERN" <<<"$log"; then
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
  echo "emulator $NAME: no result marker (PPSSPPHeadless exit $status) after ${elapsed} s of a" \
    "${TIMEOUT_SECONDS} s timeout; the EBOOT crashed or timed out. Last log lines:" >&2
  tail -n 40 <<<"$log" >&2
  exit 1
fi
echo "emulator $NAME: OK in ${elapsed} s"

#!/usr/bin/env bash
# Runs the entropy determinism probe in PPSSPPHeadless and prints the verdict. See the companion
# entropy_probe.c for what it checks and why.
# Usage: tests/security/run_entropy_probe.sh <path/to/probe/EBOOT.PBP>
set -euo pipefail

readonly EBOOT="${1:?usage: run_entropy_probe.sh <path/to/EBOOT.PBP>}"
readonly TIMEOUT_SECONDS=30
readonly STDOUT_PREFIX_PATTERN='^[A-Z] stdout: '

if [[ ! -f "$EBOOT" ]]; then
  echo "error: $EBOOT not found; build it first with scripts/dev.sh psp" >&2
  exit 1
fi

log="$(PPSSPPHeadless -l --graphics=software --timeout="$TIMEOUT_SECONDS" "$EBOOT" 2>&1 || true)"
grep -E "$STDOUT_PREFIX_PATTERN" <<<"$log" | sed -E "s/$STDOUT_PREFIX_PATTERN//" || {
  echo "entropy probe: no output; the EBOOT crashed or timed out" >&2
  tail -n 40 <<<"$log" >&2
  exit 1
}

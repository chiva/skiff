#!/usr/bin/env bash
# Hardware tier without PSPLINK: copies the debug EBOOTs to a mounted Memory Stick (PSP in USB mode
# or a card reader), then reads back the result.txt each check EBOOT writes next to itself. Plain
# file copies on the host; needs no Docker and no PSP tools. Build first with `scripts/dev.sh psp`.
# Usage: scripts/memstick.sh install|results|uninstall <memory-stick-mount>
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT
readonly USAGE="usage: scripts/memstick.sh install|results|uninstall <memory-stick-mount>"
readonly BUILD_PBP_DIR="$REPO_ROOT/build/psp/pbp"
readonly RESULT_FILE="result.txt"
# build target -> folder under PSP/GAME. The check EBOOTs write result.txt; the app does not.
readonly TARGETS=(skiff skiff_selftest skiff_tls_probe skiff_entropy_probe)
readonly FOLDERS=(Skiff SkiffSelftest SkiffTLSProbe SkiffEntropyProbe)

readonly COMMAND="${1:?$USAGE}"
readonly MOUNT="${2:?$USAGE}"
readonly GAME_DIR="$MOUNT/PSP/GAME"

if [[ ! -d "$GAME_DIR" ]]; then
  echo "error: $GAME_DIR not found; is the Memory Stick mounted at $MOUNT?" >&2
  exit 1
fi

install_eboots() {
  for i in "${!TARGETS[@]}"; do
    local source="$BUILD_PBP_DIR/${TARGETS[$i]}/EBOOT.PBP"
    local dest="$GAME_DIR/${FOLDERS[$i]}"
    if [[ ! -f "$source" ]]; then
      echo "error: $source not found; run scripts/dev.sh psp first" >&2
      exit 1
    fi
    mkdir -p "$dest"
    cp "$source" "$dest/EBOOT.PBP"
    # A stale result would be mistaken for a new run; macOS metadata files confuse the XMB.
    rm -f "$dest/$RESULT_FILE"
    find "$dest" -name '._*' -delete
    echo "installed ${TARGETS[$i]} -> PSP/GAME/${FOLDERS[$i]}"
  done
  sync
  echo "Eject the Memory Stick, then run each Skiff entry from Game > Memory Stick."
}

print_results() {
  for i in "${!TARGETS[@]}"; do
    [[ "${TARGETS[$i]}" == skiff ]] && continue
    echo "== ${FOLDERS[$i]}"
    local result="$GAME_DIR/${FOLDERS[$i]}/$RESULT_FILE"
    if [[ -f "$result" ]]; then
      cat "$result"
    else
      echo "(no $RESULT_FILE: not run yet, or it crashed before opening the file)"
    fi
  done
}

uninstall_eboots() {
  for folder in "${FOLDERS[@]}"; do
    rm -rf "${GAME_DIR:?}/$folder"
    echo "removed PSP/GAME/$folder"
  done
  sync
}

case "$COMMAND" in
install) install_eboots ;;
results) print_results ;;
uninstall) uninstall_eboots ;;
*)
  echo "$USAGE" >&2
  exit 2
  ;;
esac

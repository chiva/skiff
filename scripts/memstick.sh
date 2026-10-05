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
readonly TARGETS=(skiff skiff_selftest skiff_tls_probe skiff_kirk_probe skiff_ui_proto skiff_net_probe)
readonly FOLDERS=(Skiff SkiffSelftest SkiffTLSProbe SkiffKIRKProbe SkiffUIProto SkiffNetProbe)
# Logs some check EBOOTs append to across runs (kept by install, unlike result.txt).
readonly RUN_LOGS=(kirk-log.txt net-log.txt)
# The network probe talks to the test RomM from `scripts/dev.sh romm-lan`: it gets that server's
# address (from its certificate's addresses), the test CA and the client certificates. The keys are
# test material for that throwaway server; uninstall removes them with the folder.
readonly NET_PROBE_FOLDER="SkiffNetProbe"
readonly INTEGRATION_CERTS="$REPO_ROOT/build/integration/certs"
readonly NET_PROBE_FILES=(ca.crt client-ecdsa.crt client-ecdsa.key client-rsa.crt client-rsa.key
  client-wrong-ca.crt client-wrong-ca.key)
readonly DEFAULT_NET_PROFILE=1

readonly COMMAND="${1:?$USAGE}"
readonly MOUNT="${2:?$USAGE}"
readonly GAME_DIR="$MOUNT/PSP/GAME"

if [[ ! -d "$GAME_DIR" ]]; then
  echo "error: $GAME_DIR not found; is the Memory Stick mounted at $MOUNT?" >&2
  exit 1
fi

# macOS writes AppleDouble files ("._<name>") on FAT volumes: inside each folder, and one beside
# the folder in PSP/GAME, which the XMB can list as Corrupted Data. Only Skiff's own are removed.
remove_macos_metadata() {
  local folder="$1"
  rm -f "$GAME_DIR/._$folder"
  if [[ -d "$GAME_DIR/$folder" ]]; then
    find "$GAME_DIR/$folder" -name '._*' -delete
  fi
}

# The LAN address romm-lan put in the server certificate; empty if it only serves localhost.
test_server_lan_ip() {
  local san="$INTEGRATION_CERTS/server.san"
  [[ -f "$san" ]] || return 0
  tr ',' '\n' <"$san" | sed -n 's/^IP://p' | grep -v '^127\.' | tail -n 1 || true
}

install_net_probe_config() {
  local dest="$GAME_DIR/$NET_PROBE_FOLDER" host profile="${SKIFF_NET_PROFILE:-$DEFAULT_NET_PROFILE}"
  host="$(test_server_lan_ip)"
  if [[ -z "$host" ]]; then
    echo "note: no LAN test server; run scripts/dev.sh romm-lan and install again before running" \
      "the network probe" >&2
    return
  fi
  for file in "${NET_PROBE_FILES[@]}"; do
    cp "$INTEGRATION_CERTS/$file" "$dest/$file"
  done
  printf '%s\n' "# Written by scripts/memstick.sh install" "host=$host" "profile=$profile" \
    >"$dest/net-probe.ini"
  remove_macos_metadata "$NET_PROBE_FOLDER"
  echo "network probe: server $host, Network Settings profile $profile (SKIFF_NET_PROFILE)"
}

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
    # A stale result would be mistaken for a new run.
    rm -f "$dest/$RESULT_FILE"
    remove_macos_metadata "${FOLDERS[$i]}"
    echo "installed ${TARGETS[$i]} -> PSP/GAME/${FOLDERS[$i]}"
  done
  install_net_probe_config
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
    for log in "${RUN_LOGS[@]}"; do
      local log_path="$GAME_DIR/${FOLDERS[$i]}/$log"
      if [[ -f "$log_path" ]]; then
        echo "-- $log (one line per run)"
        cat "$log_path"
      fi
    done
  done
}

uninstall_eboots() {
  for folder in "${FOLDERS[@]}"; do
    rm -rf "${GAME_DIR:?}/$folder"
    remove_macos_metadata "$folder"
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

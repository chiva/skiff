#!/usr/bin/env bash
# Hardware tier without PSPLINK: copies the debug EBOOTs (and the app's CA bundle) to a mounted
# Memory Stick (PSP in USB mode or a card reader), then reads back the result.txt each check EBOOT
# writes next to itself, the app's log and files, and checks that no log holds the app's secrets.
# Plain file copies on the host; needs no Docker and no PSP tools. Build first with
# `scripts/dev.sh psp`.
# Usage: scripts/memstick.sh install|results|uninstall <memory-stick-mount>
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT
readonly USAGE="usage: scripts/memstick.sh install|results|uninstall <memory-stick-mount>"
readonly BUILD_PBP_DIR="$REPO_ROOT/build/psp/pbp"
readonly RESULT_FILE="result.txt"
# build target -> folder under PSP/GAME. The check EBOOTs write result.txt; the app does not.
readonly TARGETS=(skiff skiff_selftest skiff_tls_probe skiff_kirk_probe skiff_ui_proto skiff_net_probe
  skiff_bench skiff_resume_probe skiff_jobs_probe)
readonly FOLDERS=(Skiff SkiffSelftest SkiffTLSProbe SkiffKIRKProbe SkiffUIProto SkiffNetProbe
  SkiffBench SkiffResumeProbe SkiffJobsProbe)
# The app gets the CA bundle it trusts by default next to its EBOOT, where scripts/dev.sh psp put it.
readonly APP_TARGET="skiff"
readonly APP_FOLDER="Skiff"
readonly CA_BUNDLE_NAME="cacert.pem"
# With a LAN test server, the app gets a config.ini naming only that server, the CAs it trusts, a
# custom header and debug logging: it pairs for real on its first launch. The CA file is the
# default bundle plus the test CA, so the app parses as many certificates as it does by default
# (hardware row A1 measures that). The header's value is a random secret that must never reach a
# log, like the token pairing writes; results looks for both. A config.ini already naming the same
# server is kept, with its pairing and network.
readonly APP_CONFIG="config.ini"
readonly APP_CA_FILE="test-ca-bundle.pem"
readonly APP_TEST_HEADER="X-Skiff-Test"
readonly APP_LOG_LEVEL="debug"
readonly TEST_SERVER_TLS_PORT=8443
# What the app writes next to its EBOOT, printed by results. result.txt there comes from the
# launch check (tests/hardware/launch_check.c), a game the app downloads: its disc is read-only.
readonly APP_FILES=(installed.json queue.json)
readonly SECRET_RANDOM_BYTES=16
# Logs some check EBOOTs append to across runs (kept by install, unlike result.txt).
readonly RUN_LOGS=(kirk-log.txt net-log.txt bench-log.txt resume-log.txt jobs-log.txt skiff.log)
# The network probe talks to the test RomM from `scripts/dev.sh romm-lan`: it gets that server's
# address (from its certificate's addresses), the test CA and the client certificates. The keys are
# test material for that throwaway server; uninstall removes them with the folder.
readonly NET_PROBE_FOLDER="SkiffNetProbe"
readonly INTEGRATION_CERTS="$REPO_ROOT/build/integration/certs"
readonly NET_PROBE_FILES=(ca.crt client-ecdsa.crt client-ecdsa.key client-rsa.crt client-rsa.key
  wrong-ca.crt client-wrong-ca.crt client-wrong-ca.key)
# The benchmark downloads from the same server: the same settings, and only the test CA. Optional
# SKIFF_BENCH_RUNS, SKIFF_BENCH_SECTIONS and SKIFF_BENCH_CLOCK_MHZ become runs=, sections= and
# clock_mhz= (tests/hardware/bench.c).
readonly BENCH_FOLDER="SkiffBench"
readonly BENCH_FILES=(ca.crt)
# The resume probe downloads the seeded file too, best a large one (SKIFF_PAYLOAD_BYTES=67108864 for
# romm-lan). Optional SKIFF_RESUME_SCENARIOS, SKIFF_RESUME_WAIT_S and SKIFF_RESUME_AWAKE_S become
# scenarios=, wait_s= and awake_s= (tests/hardware/resume_probe.c).
readonly RESUME_FOLDER="SkiffResumeProbe"
readonly RESUME_FILES=(ca.crt)
# The jobs probe downloads the seeded file through the download queue's worker thread (a large one
# too). Optional SKIFF_JOBS_WAIT_S and SKIFF_JOBS_UI become wait_s= and ui=
# (tests/hardware/jobs_probe.c). A session installs SKIFF_JOBS_UI=0 for its first run, then
# SKIFF_JOBS_UI=1: the second run's speed is judged against the first's, read from jobs-log.txt.
readonly JOBS_FOLDER="SkiffJobsProbe"
readonly JOBS_FILES=(ca.crt)
readonly INTEGRATION_ENV="$REPO_ROOT/build/integration/romm.env"
# The seeded file and the API token, for the probe's download checks through Skiff's transport.
readonly INTEGRATION_SEED="$REPO_ROOT/build/integration/romm.json"
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

# json_field <name>: a value from romm.json, a single line written by tests/integration/seed.py.
# Plain sed rather than jq, which this host-only script cannot assume is installed. Only top-level
# fields: the nested "extra" file repeats the names (size, crc32...) and is dropped first.
json_field() {
  sed -E 's/, *"extra": *\{[^}]*\}//' "$INTEGRATION_SEED" |
    sed -nE "s/.*\"$1\": *\"?([^\",}]*)\"?[,}].*/\1/p"
}

# ini_value <file> <section> <key>: a config.ini value (section and key ignore case, as the app's
# parser does), empty when absent.
ini_value() {
  awk -v section="$2" -v key="$3" '
    { sub(/\r$/, "") }
    /^[[:space:]]*\[/ { current = tolower($0); gsub(/[][[:space:]]/, "", current); next }
    current == tolower(section) && index($0, "=") > 0 {
      name = substr($0, 1, index($0, "=") - 1)
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", name)
      if (tolower(name) == tolower(key)) {
        value = substr($0, index($0, "=") + 1)
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
        print value
        exit
      }
    }' "$1"
}

random_hex() {
  od -An -tx1 -N"$SECRET_RANDOM_BYTES" /dev/urandom | tr -d ' \n'
}

# Percent-encodes a file name for a URL path.
url_encode() {
  local text="$1" encoded="" char i
  for ((i = 0; i < ${#text}; i++)); do
    char="${text:i:1}"
    case "$char" in
    [A-Za-z0-9._~-]) encoded+="$char" ;;
    *) encoded+="$(printf '%%%02X' "'$char")" ;;
    esac
  done
  printf '%s' "$encoded"
}

# install_probe_config <folder> <config file> <extra lines> <certificate files...>: the test
# server's settings and certificates for one probe folder, or none if no LAN server is running.
install_probe_config() {
  local folder="$1" config="$2" extra="$3"
  shift 3
  local dest="$GAME_DIR/$folder" host profile="${SKIFF_NET_PROFILE:-$DEFAULT_NET_PROFILE}" file
  host="$(test_server_lan_ip)"
  if [[ -z "$host" ]]; then
    # A previous install's address and certificates would point the probe at another server.
    rm -f "$dest/$config"
    for file in "$@"; do
      rm -f "$dest/$file"
    done
    echo "note: no LAN test server; run scripts/dev.sh romm-lan and install again before running" \
      "$folder" >&2
    return
  fi
  for file in "$@"; do
    cp "$INTEGRATION_CERTS/$file" "$dest/$file"
  done
  # Plain HTTP reaches the LAN only when romm-lan ran with SKIFF_LAN_PLAIN_HTTP=1.
  local plain_http=0
  if grep -qx 'ROMM_PLAIN_BIND_ADDRESS=0.0.0.0' "$INTEGRATION_ENV" 2>/dev/null; then
    plain_http=1
  fi
  {
    printf '%s\n' "# Written by scripts/memstick.sh install" "host=$host" "profile=$profile" \
      "plain_http=$plain_http" "token=$(json_field token)" "rom_id=$(json_field rom_id)" \
      "file_name=$(url_encode "$(json_field file_name)")" "size=$(json_field size)" \
      "crc32=$(json_field crc32)"
    if [[ -n "$extra" ]]; then
      printf '%s\n' "$extra"
    fi
  } >"$dest/$config"
  remove_macos_metadata "$folder"
  echo "$folder: server $host, Network Settings profile $profile (SKIFF_NET_PROFILE)," \
    "plain HTTP comparison $([[ $plain_http == 1 ]] && echo on || echo off)," \
    "file $(json_field size) bytes"
}

install_probe_configs() {
  install_probe_config "$NET_PROBE_FOLDER" net-probe.ini "" "${NET_PROBE_FILES[@]}"
  # A string, not an array: macOS's Bash 3.2 rejects an empty array under set -u.
  local bench_extra=""
  if [[ -n "${SKIFF_BENCH_RUNS:-}" ]]; then
    bench_extra+="runs=$SKIFF_BENCH_RUNS"$'\n'
  fi
  if [[ -n "${SKIFF_BENCH_SECTIONS:-}" ]]; then
    bench_extra+="sections=$SKIFF_BENCH_SECTIONS"$'\n'
  fi
  if [[ -n "${SKIFF_BENCH_CLOCK_MHZ:-}" ]]; then
    bench_extra+="clock_mhz=$SKIFF_BENCH_CLOCK_MHZ"$'\n'
  fi
  install_probe_config "$BENCH_FOLDER" bench.ini "${bench_extra%$'\n'}" "${BENCH_FILES[@]}"
  local resume_extra=""
  if [[ -n "${SKIFF_RESUME_SCENARIOS:-}" ]]; then
    resume_extra+="scenarios=$SKIFF_RESUME_SCENARIOS"$'\n'
  fi
  if [[ -n "${SKIFF_RESUME_WAIT_S:-}" ]]; then
    resume_extra+="wait_s=$SKIFF_RESUME_WAIT_S"$'\n'
  fi
  if [[ -n "${SKIFF_RESUME_AWAKE_S:-}" ]]; then
    resume_extra+="awake_s=$SKIFF_RESUME_AWAKE_S"$'\n'
  fi
  install_probe_config "$RESUME_FOLDER" resume-probe.ini "${resume_extra%$'\n'}" "${RESUME_FILES[@]}"
  local jobs_extra=""
  if [[ -n "${SKIFF_JOBS_WAIT_S:-}" ]]; then
    jobs_extra+="wait_s=$SKIFF_JOBS_WAIT_S"$'\n'
  fi
  if [[ -n "${SKIFF_JOBS_UI:-}" ]]; then
    jobs_extra+="ui=$SKIFF_JOBS_UI"$'\n'
  fi
  install_probe_config "$JOBS_FOLDER" jobs-probe.ini "${jobs_extra%$'\n'}" "${JOBS_FILES[@]}"
}

install_app_config() {
  local dest="$GAME_DIR/$APP_FOLDER" host url
  host="$(test_server_lan_ip)"
  if [[ -z "$host" ]]; then
    echo "note: no LAN test server; the app keeps its config.ini (run scripts/dev.sh romm-lan and" \
      "install again to point it there)" >&2
    return
  fi
  url="https://$host:$TEST_SERVER_TLS_PORT"
  # Every romm-lan makes a new test CA, so the bundle is rewritten even for a kept config.ini.
  cat "$dest/$CA_BUNDLE_NAME" "$INTEGRATION_CERTS/ca.crt" >"$dest/$APP_CA_FILE"
  if [[ -f "$dest/$APP_CONFIG" && "$(ini_value "$dest/$APP_CONFIG" server url)" == "$url" ]]; then
    echo "$APP_FOLDER: config.ini for $url kept (pairing and network too)"
    return
  fi
  printf '%s\n' "# Written by scripts/memstick.sh install for the test RomM" "[server]" "url = $url" \
    "ca_file = $APP_CA_FILE" "" "[headers]" "$APP_TEST_HEADER = $(random_hex)" "" "[log]" \
    "level = $APP_LOG_LEVEL" >"$dest/$APP_CONFIG"
  echo "$APP_FOLDER: new config.ini for $url; the app pairs on its first launch"
}

install_ca_bundle() {
  local source="$BUILD_PBP_DIR/$APP_TARGET/$CA_BUNDLE_NAME"
  if [[ ! -f "$source" ]]; then
    echo "error: $source not found; run scripts/dev.sh psp first" >&2
    exit 1
  fi
  cp "$source" "$1/$CA_BUNDLE_NAME"
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
    if [[ "${TARGETS[$i]}" == "$APP_TARGET" ]]; then
      install_ca_bundle "$dest"
    fi
    # A stale result would be mistaken for a new run.
    rm -f "$dest/$RESULT_FILE"
    remove_macos_metadata "${FOLDERS[$i]}"
    echo "installed ${TARGETS[$i]} -> PSP/GAME/${FOLDERS[$i]}"
  done
  install_app_config
  install_probe_configs
  sync
  echo "Eject the Memory Stick, then run each Skiff entry from Game > Memory Stick."
}

# The app's secrets (the token pairing wrote, the test header's value) must appear in no file Skiff
# folders hold but config.ini. Names what leaked where, never the value; 1 if anything did.
check_app_secrets() {
  local config="$GAME_DIR/$APP_FOLDER/$APP_CONFIG" value leaks i
  local sections=(auth headers) keys=(token "$APP_TEST_HEADER")
  echo "== secrets"
  if [[ ! -f "$config" ]]; then
    echo "(no $APP_CONFIG in PSP/GAME/$APP_FOLDER)"
    return 0
  fi
  local status=0 checked=0
  for i in "${!keys[@]}"; do
    value="$(ini_value "$config" "${sections[$i]}" "${keys[$i]}")"
    if [[ -z "$value" ]]; then
      echo "[${sections[$i]}] ${keys[$i]}: not in $APP_CONFIG (not paired yet, or not written by install)"
      continue
    fi
    checked=$((checked + 1))
    leaks="$(grep -rlF --exclude="$APP_CONFIG" -e "$value" "$GAME_DIR"/Skiff* || true)"
    if [[ -n "$leaks" ]]; then
      echo "FAIL [${sections[$i]}] ${keys[$i]} found in:"
      printf '%s\n' "$leaks" | sed 's/^/  /'
      status=1
    fi
  done
  if [[ "$status" == 0 ]]; then
    echo "ok   $checked secret value(s) from $APP_CONFIG in no other file under PSP/GAME/Skiff*"
  fi
  return "$status"
}

print_results() {
  for i in "${!TARGETS[@]}"; do
    echo "== ${FOLDERS[$i]}"
    local result="$GAME_DIR/${FOLDERS[$i]}/$RESULT_FILE"
    if [[ "${TARGETS[$i]}" == "$APP_TARGET" ]]; then
      local file
      for file in "${APP_FILES[@]}"; do
        if [[ -f "$GAME_DIR/${FOLDERS[$i]}/$file" ]]; then
          echo "-- $file"
          cat "$GAME_DIR/${FOLDERS[$i]}/$file"
          echo
        fi
      done
      if [[ -f "$result" ]]; then
        echo "-- $RESULT_FILE (the launch check, a game Skiff downloaded)"
        cat "$result"
      fi
    elif [[ -f "$result" ]]; then
      cat "$result"
    else
      echo "(no $RESULT_FILE: not run yet, or it crashed before opening the file)"
    fi
    for log in "${RUN_LOGS[@]}"; do
      local log_path="$GAME_DIR/${FOLDERS[$i]}/$log"
      if [[ -f "$log_path" ]]; then
        echo "-- $log (appended by every run)"
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
results)
  print_results
  check_app_secrets
  ;;
uninstall) uninstall_eboots ;;
*)
  echo "$USAGE" >&2
  exit 2
  ;;
esac

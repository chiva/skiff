#!/usr/bin/env bash
# One entry point for every local build and check. Everything runs in containers, so Docker is the
# only prerequisite and results match CI. Run `scripts/dev.sh help` for the command list.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPO_ROOT
readonly TOOLCHAIN_IMAGE="skiff-toolchain"
readonly HOST_IMAGE="skiff-host"
readonly PPSSPP_IMAGE="skiff-ppsspp"
# The UI prototype draws a full screen for 10 emulated seconds in the software renderer, which can
# outlast the emulator's default 30 s on a slow host (tests/emulator/run_eboot.sh). CI passes the same
# value in its UI prototype step.
readonly UI_PROTO_TIMEOUT_SECONDS=90
# The app smoke test draws its first screen for 60 frames in the same software renderer, after
# reading its files; CI passes the same value in its app step.
readonly APP_SMOKE_TIMEOUT_SECONDS=90
readonly COVERAGE_FLOOR=85
# CI runs `test` and `asan` through this script, so this list is the compiler matrix everywhere.
readonly HOST_COMPILERS=(gcc clang)
# The integration RomM (tests/integration/): generated certificates, secrets and seed results.
readonly INTEGRATION_DIR="build/integration"
readonly COMPOSE_FILE="$REPO_ROOT/tests/integration/compose.yaml"
readonly COMPOSE_PROJECT="skiff-romm"
readonly COMPOSE_NETWORK="${COMPOSE_PROJECT}_default"
readonly ROMM_ADMIN_USER="skiff"
# The fake transport's recorded RomM responses (tests/support/fake_transport.h), and the size of the
# synthetic file seeded while recording them, kept small so the fixture stays small.
readonly FIXTURES_DIR="tests/fixtures/romm"
readonly FIXTURE_PAYLOAD_BYTES=4096
# The app's build target, and the CA bundle the toolchain image holds in $SKIFF_CA_BUNDLE_DIR
# (docker/ca-bundle/fetch-ca-bundle.sh). PSP builds copy it next to the app's EBOOT: the app reads
# it from its own folder (SKIFF_APP_DEFAULT_CA_FILE), and scripts/memstick.sh and package take it
# from there.
readonly APP_TARGET="skiff"
readonly CA_BUNDLE_NAME="cacert.pem"

# Bind mounts for every container. In a git worktree, .git is a file pointing at the main
# repository's .git directory by absolute host path; mounting that directory read-only at the same
# path lets git (lint.sh's file list) work inside the containers as it does in a normal checkout.
SOURCE_MOUNTS=(-v "$REPO_ROOT":/src)
GIT_COMMON_DIR="$(git -C "$REPO_ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
if [[ -n "$GIT_COMMON_DIR" && "$GIT_COMMON_DIR" != "$REPO_ROOT/.git" ]]; then
  SOURCE_MOUNTS+=(-v "$GIT_COMMON_DIR":"$GIT_COMMON_DIR":ro)
fi
readonly SOURCE_MOUNTS

usage() {
  cat <<'EOF'
Usage: scripts/dev.sh <command> [<command>...]

Commands run in the order given and stop at the first failure.

  psp          Build the debug EBOOTs        -> build/psp/pbp/ (the app's with cacert.pem)
  psp-release  Build the release EBOOTs      -> build/psp-release/pbp/
  package      Build release and zip it      -> dist/skiff-<version>.zip (checks its contents)
  test         Host unit tests, gcc and clang  -> build/host-{gcc,clang}/
  asan         Host unit tests under ASan + UBSan, gcc and clang
  coverage     Host unit tests with coverage (fails under 85% line coverage)
  lint         clang-tidy and cppcheck over first-party sources
  selftest     Run the self-test EBOOT in PPSSPPHeadless (needs `psp` first)
  tls-probe    Run the TLS toolchain probe in PPSSPPHeadless (needs `psp` first)
  kirk-probe   Run the KIRK probe in PPSSPPHeadless, which has no ARK: TLS must refuse to start
               (needs `psp` first; the measurements need a real PSP)
  net-probe    Run the network probe in PPSSPPHeadless, which has no ARK: the network modules must
               load and unload, and TLS must refuse (needs `psp` first; the requests need a PSP)
  ui-proto     Run the UI prototype in PPSSPPHeadless: with no input it renders, checks its fonts
               and exits on its own (needs `psp` first; the dialogs need a real PSP)
  bench        Run the benchmark in PPSSPPHeadless, which has no ARK: TLS must refuse, the CRC-32
               and Memory Stick code runs on small sizes (needs `psp` first; numbers need a PSP)
  resume-probe Run the resume probe in PPSSPPHeadless, which has no ARK: the PSP storage (sceIo)
               the downloads write through must work, and TLS must refuse (needs `psp` first; the
               interruptions need a real PSP)
  jobs-probe   Run the jobs probe in PPSSPPHeadless, which has no ARK: a queued job must fail on
               the worker thread before any network I/O, and the queue file and log must say so
               (needs `psp` first; the downloads and interruptions need a real PSP)
  app-smoke    Run the app's smoke test in PPSSPPHeadless, which has no ARK and no config.ini: the
               app must start, reach the screen asking for the server address, draw it, and tear
               down (needs `psp` first; pairing, browsing and downloads need a real PSP)
  romm-up      Start a fresh test RomM behind a TLS proxy on 127.0.0.1 (tests/integration/)
  romm-lan     The same, reachable from a PSP on the LAN (IP detected, or set SKIFF_LAN_IP)
  romm-check   Check the running test RomM: TLS, client certificates, token, ranged download
  romm-test    Run the host build's transport and resumable downloads against the running test
               RomM (TLS, mTLS, clock, keep-alive, ranged and resumed downloads)
  romm-record  Start a fresh test RomM with a small seeded file, record the fake transport's
               fixtures into tests/fixtures/romm/, and stop it
  romm-down    Stop the test RomM and delete its data
  icons        Render the icon PNGs from the SVG masters in assets/brand/
  clean        Remove build/ and dist/
EOF
}

ensure_toolchain_image() {
  # pspdev publishes amd64 images only; Apple Silicon runs them under emulation. The first build
  # compiles mbedtls and curl and takes minutes there; later runs are cached until the Dockerfile
  # changes.
  echo "toolchain image: building if docker/toolchain.Dockerfile changed..." >&2
  docker build --quiet --platform linux/amd64 -t "$TOOLCHAIN_IMAGE" \
    -f "$REPO_ROOT/docker/toolchain.Dockerfile" "$REPO_ROOT/docker" >/dev/null
}

# The toolchain command that builds a PSP preset and puts the CA bundle next to the app's EBOOT.
psp_build_command() {
  local preset="$1"
  printf '%s' "cmake --preset $preset >/dev/null && cmake --build --preset $preset && install -m 644 \
    \"\$SKIFF_CA_BUNDLE_DIR/$CA_BUNDLE_NAME\" build/$preset/pbp/$APP_TARGET/$CA_BUNDLE_NAME"
}

run_toolchain() {
  ensure_toolchain_image
  docker run --rm --platform linux/amd64 "${SOURCE_MOUNTS[@]}" -w /src "$TOOLCHAIN_IMAGE" bash -c "$1"
}

# The third argument, when given, is the emulator's timeout in seconds (tests/emulator/run_eboot.sh).
run_emulator() {
  local eboot="$1" name="$2"
  ensure_ppsspp_image
  docker run --rm "${SOURCE_MOUNTS[@]}" -w /src "$PPSSPP_IMAGE" tests/emulator/run_eboot.sh "$eboot" \
    "$name" "${@:3}"
}

ensure_host_image() {
  docker build --quiet -t "$HOST_IMAGE" -f "$REPO_ROOT/docker/host.Dockerfile" "$REPO_ROOT/docker" >/dev/null
}

ensure_ppsspp_image() {
  docker build --quiet -t "$PPSSPP_IMAGE" -f "$REPO_ROOT/docker/ppsspp.Dockerfile" "$REPO_ROOT/docker" >/dev/null
}

run_host() {
  ensure_host_image
  docker run --rm "${SOURCE_MOUNTS[@]}" -w /src "$HOST_IMAGE" bash -c "$1"
}

# The ports are global, so there is one test RomM per Docker host. Its containers record the
# checkout that started it; empty when none is running.
romm_owner() {
  docker ps -a --filter "label=com.docker.compose.project=$COMPOSE_PROJECT" \
    --format '{{.Label "com.docker.compose.project.working_dir"}}' | head -n 1
}

# Refuses to act on a test RomM another checkout (a parallel worktree) started.
require_own_romm() {
  local owner
  owner="$(romm_owner)"
  if [[ -n "$owner" && "$owner" != "$(dirname "$COMPOSE_FILE")" ]]; then
    echo "error: the test RomM running now was started from ${owner%/tests/integration};" \
      "run scripts/dev.sh romm-down there first" >&2
    exit 1
  fi
}

run_host_in_romm_network() {
  if [[ -z "$(romm_owner)" ]]; then
    echo "error: the test RomM is not running; start it with scripts/dev.sh romm-up" >&2
    exit 1
  fi
  require_own_romm
  ensure_host_image
  docker run --rm --network "$COMPOSE_NETWORK" "${SOURCE_MOUNTS[@]}" -w /src "$HOST_IMAGE" bash -c "$1"
}

compose() {
  docker compose --progress quiet -f "$COMPOSE_FILE" --env-file "$REPO_ROOT/$INTEGRATION_DIR/romm.env" "$@"
}

random_hex() {
  od -An -tx1 -N32 /dev/urandom | tr -d ' \n'
}

# The address a PSP on the same network can reach this machine at.
lan_ip() {
  if [[ -n "${SKIFF_LAN_IP:-}" ]]; then
    echo "$SKIFF_LAN_IP"
    return
  fi
  case "$(uname -s)" in
  Darwin) ipconfig getifaddr "$(route -n get default | awk '/interface:/ {print $2}')" ;;
  *) ip -4 route get 1.1.1.1 | awk '{for (i = 1; i < NF; i++) if ($i == "src") print $(i + 1)}' ;;
  esac
}

# romm_up <bind-address> [<lan-ip>]: always a fresh server (empty volumes, new secrets), so every run
# starts from the same state. Plain HTTP is published on the LAN only with SKIFF_LAN_PLAIN_HTTP=1.
romm_up() {
  local bind_address="$1" lan_ip="${2:-}" dir="$REPO_ROOT/$INTEGRATION_DIR" admin_password host
  local plain_bind_address="127.0.0.1" plain_host
  if [[ -n "$lan_ip" && "${SKIFF_LAN_PLAIN_HTTP:-0}" == 1 ]]; then
    plain_bind_address="$bind_address"
  fi
  require_own_romm
  mkdir -p "$dir"
  # As the invoking user, so on a Linux host the keys to copy to a PSP are readable by that user.
  ensure_host_image
  docker run --rm --user "$(id -u):$(id -g)" "${SOURCE_MOUNTS[@]}" -w /src "$HOST_IMAGE" \
    tests/integration/gen-certs.sh "$INTEGRATION_DIR/certs" "$lan_ip"
  admin_password="$(random_hex)"
  (
    umask 077
    printf '%s\n' "ROMM_DB_PASSWORD=$(random_hex)" "ROMM_AUTH_SECRET_KEY=$(random_hex)" \
      "ROMM_BIND_ADDRESS=$bind_address" "ROMM_PLAIN_BIND_ADDRESS=$plain_bind_address" \
      "SKIFF_CERTS_DIR=$dir/certs" \
      "SKIFF_ADMIN_USER=$ROMM_ADMIN_USER" "SKIFF_ADMIN_PASSWORD=$admin_password" >"$dir/romm.env"
  )
  echo "test RomM: starting from empty volumes (about a minute)..." >&2
  compose down --volumes --remove-orphans
  compose up --detach --wait
  (
    umask 077
    compose exec -T -e SKIFF_ADMIN_USER="$ROMM_ADMIN_USER" -e SKIFF_ADMIN_PASSWORD="$admin_password" \
      ${SKIFF_PAYLOAD_BYTES:+-e SKIFF_PAYLOAD_BYTES="$SKIFF_PAYLOAD_BYTES"} \
      romm python3 /seed.py >"$dir/romm.json"
  )
  host="${lan_ip:-localhost}"
  plain_host="localhost"
  if [[ "$plain_bind_address" != 127.0.0.1 ]]; then
    plain_host="$host"
  fi
  cat <<EOF
Test RomM is up (web UI login: $ROMM_ADMIN_USER, password in $INTEGRATION_DIR/romm.env):
  http://$plain_host:8080   plain HTTP, for comparison only
  https://$host:8443  TLS, trusted through $INTEGRATION_DIR/certs/ca.crt
  https://$host:8444  TLS + client certificate ($INTEGRATION_DIR/certs/client-{ecdsa,rsa}.{crt,key})
Seeded file, its hashes and an API token: $INTEGRATION_DIR/romm.json
EOF
}

# By project name, so it works even when build/integration/ is gone.
romm_down() {
  require_own_romm
  docker compose --progress quiet -p "$COMPOSE_PROJECT" down --volumes --remove-orphans
}

host_tests() {
  local preset="$1" compiler
  for compiler in "${HOST_COMPILERS[@]}"; do
    run_host "scripts/host-tests.sh $preset $compiler"
  done
}

run_command() {
  local cmd="$1"
  case "$cmd" in
  psp | psp-release)
    run_toolchain "$(psp_build_command "$cmd")"
    ;;
  package)
    run_toolchain "$(psp_build_command psp-release) \
      && scripts/collect-licenses.sh build/psp-release/licenses >/dev/null"
    "$REPO_ROOT/scripts/package.sh" "$REPO_ROOT/build/psp-release/pbp/$APP_TARGET/EBOOT.PBP" \
      "$REPO_ROOT/build/psp-release/pbp/$APP_TARGET/$CA_BUNDLE_NAME" \
      "$REPO_ROOT/build/psp-release/licenses/third-party-licenses"
    ;;
  test)
    host_tests host
    ;;
  asan)
    host_tests host-asan
    ;;
  coverage)
    run_host "cmake --preset host-coverage >/dev/null && cmake --build --preset host-coverage \
      && ctest --preset host-coverage"
    run_host "gcovr --root . --filter src/ --exclude src/platform/ --fail-under-line $COVERAGE_FLOOR \
      --print-summary --txt --xml build/host-coverage/coverage.xml"
    ;;
  lint)
    run_host "cmake --preset host >/dev/null && scripts/lint.sh build/host"
    ;;
  selftest)
    run_emulator build/psp/pbp/skiff_selftest/EBOOT.PBP SELFTEST
    ;;
  tls-probe)
    run_emulator build/psp/pbp/skiff_tls_probe/EBOOT.PBP "TLS PROBE"
    ;;
  kirk-probe)
    run_emulator build/psp/pbp/skiff_kirk_probe/EBOOT.PBP "KIRK PROBE NO ARK"
    ;;
  net-probe)
    run_emulator build/psp/pbp/skiff_net_probe/EBOOT.PBP "NET PROBE NO ARK"
    ;;
  ui-proto)
    run_emulator build/psp/pbp/skiff_ui_proto/EBOOT.PBP "UI PROTO HEADLESS" "$UI_PROTO_TIMEOUT_SECONDS"
    ;;
  bench)
    run_emulator build/psp/pbp/skiff_bench/EBOOT.PBP "BENCH NO ARK"
    ;;
  resume-probe)
    run_emulator build/psp/pbp/skiff_resume_probe/EBOOT.PBP "RESUME PROBE NO ARK"
    ;;
  jobs-probe)
    run_emulator build/psp/pbp/skiff_jobs_probe/EBOOT.PBP "JOBS PROBE NO ARK"
    ;;
  app-smoke)
    run_emulator build/psp/pbp/skiff_app_smoke/EBOOT.PBP "APP SMOKE" "$APP_SMOKE_TIMEOUT_SECONDS"
    ;;
  romm-up)
    romm_up 127.0.0.1
    ;;
  romm-lan)
    local ip
    ip="$(lan_ip)"
    if [[ -z "$ip" ]]; then
      echo "error: no LAN address found; set SKIFF_LAN_IP to this machine's address" >&2
      exit 1
    fi
    romm_up 0.0.0.0 "$ip"
    ;;
  romm-check)
    run_host_in_romm_network "tests/integration/check.sh $INTEGRATION_DIR"
    ;;
  romm-test)
    run_host_in_romm_network "tests/integration/transport-test.sh $INTEGRATION_DIR"
    ;;
  romm-record)
    # The server exists only for the recording: stop it even when starting or recording fails.
    trap romm_down EXIT
    SKIFF_PAYLOAD_BYTES="$FIXTURE_PAYLOAD_BYTES" romm_up 127.0.0.1
    run_host_in_romm_network "tests/integration/record-fixtures.sh $INTEGRATION_DIR $FIXTURES_DIR"
    trap - EXIT
    romm_down
    ;;
  romm-down)
    romm_down
    ;;
  icons)
    run_host scripts/render-icons.sh
    ;;
  clean)
    # Stop this checkout's test RomM first: its secrets live in build/.
    if [[ "$(romm_owner)" == "$(dirname "$COMPOSE_FILE")" ]]; then
      romm_down
    fi
    rm -rf "$REPO_ROOT/build" "$REPO_ROOT/dist"
    ;;
  help | -h | --help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
  esac
}

if [[ $# -eq 0 ]]; then
  usage
  exit 0
fi
for cmd in "$@"; do
  run_command "$cmd"
done

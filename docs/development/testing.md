# Testing

Skiff is tested at three levels. The same self-test checks (`src/core/selftest.c`) run in all
three, so a failure on hardware that does not happen on the host points at the PSP environment, not
the logic.

| Tier | Where | In CI | Command |
|---|---|---|---|
| Unit | Host (Linux/macOS) | ✅ gcc + clang, plain + ASan/UBSan, coverage ≥85% | `scripts/dev.sh test asan coverage` |
| Emulator | PPSSPPHeadless | ✅ self-test + TLS probe | `scripts/dev.sh psp selftest tls-probe` |
| Hardware | Real PSP over PSPLINK | ❌ manual | see below |

## Unit tests

- Framework: [Unity](https://github.com/ThrowTheSwitch/Unity). One executable per module in
  `tests/unit/test_<module>.c`, registered with `skiff_add_unit_test()` in `tests/CMakeLists.txt`.
- Tests narrate with `TEST_PRINTF` so a failure in CI shows what was being checked.
- Coverage counts portable code only (`src/` minus `src/platform/`).

Planned additions:

- **Contract tests**: recorded RomM 5.3 API responses in `tests/fixtures/romm/`, parsed by the real
  client code, so an API change shows up as a failing test rather than a crash on a PSP.
- **Integration tests**: a disposable RomM in Docker Compose (`tests/integration/`) seeded with
  homebrew fixtures, exercised by the host build of the client.

## Emulator tests

`tests/emulator/run_eboot.sh <EBOOT> <NAME>` boots a check EBOOT in PPSSPPHeadless and passes only
when the output contains `SKIFF <NAME> OK`. It runs `skiff_selftest` (`SELFTEST`) and
`skiff_tls_probe` (`TLS PROBE`, see [Security probes](#security-probes)). PPSSPPHeadless only shows a
program's stdout inside its full log (`-l`, lines starting `I stdout: `), so the script extracts
those lines and prints the end of the log when the marker is missing. The first local run builds the
PPSSPP image, which takes several minutes; later runs reuse it.

PPSSPP does not emulate the PSP's Wi-Fi hardware or real Memory Stick timing, so networking and
storage behaviour must also be checked on hardware.

## Hardware tier

Run before merging changes to `src/net/`, `src/storage/`, `src/platform/psp/`, or a toolchain
bump. Record the PSP model and firmware in the PR.

### From the XMB (no PSPLINK, nothing to install)

Every check EBOOT also writes its output to `result.txt` next to its `EBOOT.PBP`, flushed line by
line, so a run started from the XMB can be read back from the Memory Stick.

1. Build: `scripts/dev.sh psp`.
2. Put the PSP in USB mode (or use a card reader), then `scripts/memstick.sh install <mount>`
   (e.g. `/Volumes/PSP`). Eject.
3. On the PSP, from Game → Memory Stick, run **Skiff self-test** and **Skiff TLS probe**. Each
   returns to the XMB when done.
4. Back in USB mode: `scripts/memstick.sh results <mount>` → expect `SKIFF SELFTEST OK` and
   `SKIFF TLS PROBE OK`. A missing or truncated `result.txt` means the EBOOT crashed; its last line
   shows how far it got.
5. Run **Skiff**: check the version on screen and that START exits; then HOME → Quit must exit
   without freezing.
6. `scripts/memstick.sh uninstall <mount>` removes the folders when done (it keeps nothing else).

The **Skiff KIRK probe** is installed too; run it only when working on entropy (see
[KIRK probe](#kirk-probe)).

### Over PSPLINK

1. Set up PSPLINK ([debugging](debugging.md)) and build: `scripts/dev.sh psp`.
2. In `pspsh`: `./build/psp/skiff_selftest.prx` → expect `SKIFF SELFTEST OK`, and
   `./build/psp/skiff_tls_probe.prx` → expect `SKIFF TLS PROBE OK`.
3. Run the app: `./build/psp/skiff.prx`, check the version on screen, press START, and
   check it returns cleanly.

From the networking release on, the hardware tier adds: joining the TKIP test SSID; an HTTPS
request to a test RomM; TLS 1.3 (or 1.2 with ECDHE) confirmed in a packet capture; a proxy rejecting a
missing or wrong client certificate; a download interrupted (Wi-Fi switch off) and resumed; and
installing a homebrew ISO that then boots from ARK-5.

## Security probe

`scripts/dev.sh tls-probe` runs `tests/security/tls_probe.c` (CI runs it on every PR). It checks that
libcurl uses Mbed TLS 4.1 with HTTP and HTTPS only, and that TLS randomness comes **only** from
`mbedtls_platform_get_entropy()`: the probe's version of that hook refuses every request, and both
`psa_crypto_init()` and `curl_global_init()` must then fail after calling it. If anything else could
seed TLS, they would succeed and the probe fails.

## KIRK probe

`tests/security/kirk_probe.c` measures the KIRK crypto engine's random generator, read through ARK's
`sctrlKernelRand()`, before Skiff's entropy source is built on it. Hardware only: PPSSPP has no ARK,
so CI only builds it. Per run it reports how long a 128-byte gather (one Mbed TLS entropy request,
32 calls) takes, next to the same gather from the C library's `getentropy()` (the toolchain's
default source) as a baseline, and 1024 values checked for repeats, bit balance and byte
distribution (`SKIFF KIRK PROBE OK`/`FAIL`; the baseline is timed only, but a baseline failure or
an unwritten log fails the run). It appends a fingerprint line (uptime, the first KIRK values in
`first=`, and both timings) to `kirk-log.txt`, after comparing its `first=` values with every
earlier run in that file: a match fails the run, since it would mean KIRK produced the same sequence
twice, e.g. restarting it after every power-on. Run it several times, power-cycling the PSP in
between; a single run has nothing to compare against.
`scripts/memstick.sh results` prints both files.

## Test data rules

Homebrew, public domain or synthetic only. No commercial games, BIOS files, real saves, tokens or
keys, ever.

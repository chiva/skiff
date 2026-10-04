# Testing

Skiff is tested at three levels. The same self-test checks (`src/core/selftest.c`) run in all
three, so a failure on hardware that does not happen on the host points at the PSP environment, not
the logic.

| Tier | Where | In CI | Command |
|---|---|---|---|
| Unit | Host (Linux/macOS) | ✅ gcc + clang, plain + ASan/UBSan, coverage ≥85% | `scripts/dev.sh test asan coverage` |
| Emulator | PPSSPPHeadless | ✅ self-test, TLS probe, KIRK probe (no ARK), UI prototype (headless) | `scripts/dev.sh psp selftest tls-probe kirk-probe ui-proto` |
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
when the output contains `SKIFF <NAME> OK`. It runs `skiff_selftest` (`SELFTEST`),
`skiff_tls_probe` (`TLS PROBE`, see [Security probe](#security-probe)), `skiff_kirk_probe`
(`KIRK PROBE NO ARK`, see [KIRK probe](#kirk-probe)) and `skiff_ui_proto` (`UI PROTO HEADLESS`,
see [UI prototype](#ui-prototype)). PPSSPPHeadless only shows a
program's stdout inside its full log (`-l`, lines starting `I stdout: `), so the script extracts
those lines and prints the end of the log when the marker is missing. The first local run builds the
PPSSPP image, which takes several minutes; later runs reuse it.

PPSSPP does not emulate the PSP's Wi-Fi hardware or real Memory Stick timing, so networking and
storage behaviour must also be checked on hardware. Its timings mean nothing either: frame and load
times count only from a PSP.

`docker/ppsspp.Dockerfile` patches one line of PPSSPP: on Linux it refuses every program read of
`flash0:` (where the firmware fonts live). The build fails if the patch stops applying, so a PPSSPP
bump shows whether it is still needed.

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

The **Skiff KIRK probe** and the **Skiff UI prototype** are installed too; run them only when
working on entropy (see [KIRK probe](#kirk-probe)) or the UI (see [UI prototype](#ui-prototype)).

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
`sctrlKernelRand()`, and Skiff's entropy hook built on it. On a PSP with ARK it reports, per run:

- how long a 128-byte gather (one Mbed TLS entropy request, 32 calls) takes, next to the same gather
  from the C library's `getentropy()` (the toolchain's default source) as a baseline;
- 1024 KIRK values checked for repeats, bit balance and byte distribution (the baseline is timed
  only);
- Skiff's TLS stack seeded by the real hook: `psa_crypto_init()` and `curl_global_init()` must
  succeed (their times are reported), and two `psa_generate_random()` draws must differ;
- last, a request the hook does not support (non-zero flags) must be refused and turn the status to
  `SKIFF_ERR_NET_ENTROPY`.

It ends with `SKIFF KIRK PROBE OK`/`FAIL` (a baseline failure or an unwritten log also fails the run)
and appends a fingerprint line (uptime, the first KIRK values in `first=`, and all timings) to
`kirk-log.txt`, after comparing its `first=` values with every earlier run in that file: a match
fails the run, since it would mean KIRK produced the same sequence twice, e.g. restarting it after
every power-on. Run it several times, power-cycling the PSP in between; a single run has nothing to
compare against.
`scripts/memstick.sh results` prints both files.

Without ARK there is nothing to measure, so the probe checks instead that Skiff's hook refuses, TLS
fails closed, and the reason reported is `SKIFF_ERR_NET_NEEDS_ARK` (`SKIFF KIRK PROBE NO ARK OK`).
PPSSPP has no ARK, so that is what CI runs (`scripts/dev.sh kirk-probe`).

## UI prototype

`tests/prototype/ui_proto.c` checks the UI stack chosen in
[Architecture](architecture.md#ui-gu--intrafont) before the UI layer exists: GU draws a 20-item list
with intraFont, using the firmware's Latin font with its Japanese font as fallback, and the
on-screen keyboard and the network picker open from the render loop. It links the app's module info,
so its memory figures are the app's.

On a PSP:

1. Run **Skiff UI prototype**. Check that the title, the Japanese line and the first row (accented
   Latin: `café, señor`) render, and that Up/Down move the highlight.
2. Triangle opens the keyboard: type a few characters, confirm; the text appears top right.
3. Square loads the network modules and opens the network picker: pick a connection (or cancel);
   once connected, the IP address appears top right. The modules are unloaded when it closes.
4. HOME → Quit (or START) ends the run. Within the first 10 seconds press any button, or it exits
   on its own as described below.

`result.txt` then has: font load times; heap used by the fonts; the first frame read back (drawn
pixels in a Latin and a Japanese-only line, both must be above zero); list frame time (mean, max,
frames over the 16.7 ms budget); system memory before and after the network modules load; and the
keyboard and picker results. It ends with `SKIFF UI PROTO OK` only if both fonts rendered and the
keyboard and the picker each opened and closed (and the network modules unloaded); otherwise
`SKIFF UI PROTO FAIL`.

With no button pressed for 10 seconds it exits on its own, checking only the fonts and the frames,
and ends with `SKIFF UI PROTO HEADLESS OK`/`FAIL`. That is what CI runs (`scripts/dev.sh ui-proto`).

## Test data rules

Homebrew, public domain or synthetic only. No commercial games, BIOS files, real saves, tokens or
keys, ever.

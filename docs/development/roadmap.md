# Roadmap

Each phase ends with a release. Dates are not promised. Design decisions behind each phase are in
[Architecture](architecture.md).

## Phase 0: Foundations ✅

Repository, CMake presets, PSP and host builds, unit tests with sanitizers and coverage, emulator
self-test (PPSSPPHeadless), CI, release automation with third-party licences, user and developer
docs.

## Phase 1: Hardware spike

De-risk, on a real PSP-1000, what an emulator cannot test, and settle the open decisions:

- **Toolchain image** ✅: `pspdev/pspdev` plus curl 8.22 and Mbed TLS 4.1 LTS, built from verified
  archives (`docker/toolchain.Dockerfile`); TLS entropy routed only through Skiff's hook, checked by
  the TLS probe in CI.
- **Entropy** ✅: the KIRK hardware generator through ARK's `sctrlKernelRand()`, behind
  `mbedtls_platform_get_entropy()` with SP 800-90B health tests. On a PSP-1000 with ARK-5 its output
  never repeated across launches and power cycles, so nothing is mixed in; seeding Mbed TLS takes
  about 11 ms at startup. See [Architecture](architecture.md#randomness-for-tls).
- **Test server** ✅: a disposable RomM 5.3 behind a TLS proxy, with client certificates
  (`tests/integration/`), checked in CI. See [Testing](testing.md#integration-server).
- **Network** ✅: Skiff joins a saved Network Settings profile without the network picker (7.4 s
  on a PSP-1000) and reaches RomM over TLS 1.3 through a proxy, with or without a client
  certificate (`tests/hardware/net_probe.c`). The picker also works from the UI loop. Certificate
  dates are checked against the PSP's real-time clock: the C library's `time()` has no date there.
- **Transport layer** ✅: the network seam the app and `romm/` use (`include/skiff/transport.h`):
  keep-alive, resuming with `Range` and `If-Range`, and one error code per failure, with the same
  curl and Mbed TLS on the host as on the PSP. Tested on the host with a fake transport replaying
  recorded RomM responses and against the test server in CI, and on a PSP-1000: a resumed download
  matches byte for byte, and a clock reset to 2000 is reported as a wrong date (108). See
  [Architecture](architecture.md#transport).
- **UI stack** ✅: GU + intraFont with the firmware's Latin font, the on-screen keyboard and the
  network picker, confirmed on hardware (`tests/prototype/ui_proto.c`).
- **Measure**:
  - free memory after loading the network modules ✅: they take 600 KB of system memory, leaving
    164 KB (148 KB once connected); TLS itself uses heap, not system memory;
  - TLS handshake time ✅: 0.59 s, 0.69 s with an ECDSA P-256 client certificate, 2.07 s with
    RSA-2048;
  - Wi-Fi throughput and Memory Stick write speed by buffer size;
  - hashing speed for the hashes RomM records per file (CRC32, MD5 and SHA-1).
- **Resume**: a ranged download survives the Wi-Fi switch and a suspend.

Output: `docs/development/hardware-findings.md` with the numbers and the decisions they settle.

## Phase 2: First usable release (PSP games)

- Pairing with RomM, version check.
- PSP library with installed status (manifest), game details.
- Worker thread and persistent download queue: resume, integrity check, progress, no auto-sleep,
  recovery after suspend.
- Settings (server, CA, mTLS), log file, English and Spanish.

## Phase 3: Polish

Cover art (cached on the Memory Stick), favourites and collections, "download all favourites",
backlight dimming during long downloads.

## Phase 4: PS1

Decide how `.bin/.cue/.chd` becomes `EBOOT.PBP` (server-side conversion vs on-device packaging),
then add the PS1 installer.

## Phase 5: Save sync

PSP saves to and from RomM via argosy-sigil and `/api/sync/negotiate`, compatible with RomM's web
player and PPSSPP. No silent overwrites.

## Phase 6: More systems

Installers for emulated systems through PSP emulators, driven by
[platform requests](https://github.com/chiva/skiff/issues?q=label%3Aplatform).

## Also planned

- A documentation site (GitHub Pages) built from `docs/`.
- Proposing Skiff to the RomM project once it is usable, as other community clients have been.

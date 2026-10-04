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
- **Entropy**: kernel module reading the KIRK hardware generator, loaded through ARK; the pool
  behind `mbedtls_platform_get_entropy()` (release blocker). Must answer at startup with full
  entropy, see [Architecture](architecture.md#randomness-for-tls).
- **Network**: join a WPA/TKIP 2.4 GHz network through `sceUtilityNetconf`; HTTPS to RomM.
- **UI stack**: confirm GU + intraFont with a prototype that also opens the on-screen keyboard.
- **Measure**:
  - free memory after loading the network modules;
  - TLS handshake time with ECDSA vs RSA client certificates;
  - Wi-Fi throughput and Memory Stick write speed by buffer size;
  - hashing speed for the algorithms RomM provides.
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

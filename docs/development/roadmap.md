# Roadmap

Each phase ends with a release. Dates are not promised. Design decisions behind each phase are in
[Architecture](architecture.md).

## Phase 0: Foundations ✅

Repository, CMake presets, PSP and host builds, unit tests with sanitizers and coverage, emulator
self-test (PPSSPPHeadless), CI, release automation with third-party licences, user and developer
docs.

## Phase 1: Hardware spike ✅

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
  - Wi-Fi throughput ✅: 462 KB/s over HTTPS with the CPU at 333 MHz (349 KB/s at 222 MHz, where
    TLS is CPU-bound), close to the radio's 500 KB/s; buffer sizes make no difference;
  - Memory Stick write speed ✅: 9–13 MB/s on its own, but Wi-Fi data stops arriving while it
    writes, so downloads write in 1 MiB blocks: 410 KB/s for a 64 MiB download, against 296 KB/s
    with 128 KB blocks;
  - hashing speed ✅: zlib's CRC-32 runs at 33 MB/s (MD5 11 MB/s, SHA-1 8.2 MB/s), so the CRC-32
    RomM records is checked while downloading.
- **Resume** ✅: a download continues from its `.part` file after the Wi-Fi switch, a suspend, a
  restart or a quit, and is checked against RomM's CRC-32 (`include/skiff/download.h`,
  `tests/hardware/resume_probe.c`). On a PSP-1000 with a 64 MiB file: the switch was noticed 1.7 s
  after the last byte and the profile rejoined in 13 s; after a suspend, rejoining the profile was
  enough (8 s, the CPU stayed at 333 MHz); the HOME menu did not pause the download; keep-awake
  held off Auto Sleep. Files left open across a suspend stop working, so every attempt opens its
  own. See [Architecture](architecture.md#threads-power-and-suspend).

The numbers and the decisions they settle are in [Hardware findings](hardware-findings.md), with
the questions carried into Phase 2.

## Phase 2: First usable release (PSP games) ✅

Released as 0.2.0 (2026-10-09), checked on a PSP-1000 against a real RomM; requests to RomM run
on their own thread since (#67).

- Pairing with RomM, version check.
- PSP library with installed status (manifest), game details.
- Worker thread and persistent download queue: resume, integrity check, progress, no auto-sleep,
  recovery after suspend.
- Settings (server, CA, mTLS), log file, English and Spanish.

## Phase 3: Polish ✅

- **Cover art** ✅: a game's cover beside its details, from RomM's small cover (PNG, decoded and
  scaled on the browsing thread) and kept on the Memory Stick (64 covers, about 4.5 MB). On a
  PSP-1000 a cover decodes in about 0.3 s and reads back from the Memory Stick in about 40 ms, and
  loading one never holds up the screen. See [Architecture](architecture.md#covers).
- **Favourites** and **"download all favourites"** ✅: SELECT switches the library to the player's
  favourites (marked in RomM's web app; RomM serves them to Skiff's `roms.read` token, so no new
  pairing), and START queues every favourite that fits, with one save of the queue. On a PSP-1000,
  checking 36 favourites takes about half a second and queueing 34 of them holds up the screen
  for one frame at most. See [Hardware findings](hardware-findings.md#favourites).
  **Collections**: SELECT also lists the player's collections (regular and smart, their own and other
  users' public ones); opening one lists its PSP games, and START downloads them as it does the
  favourites. Listing them needs `collections.read`, which Phase 4's pairing asks for as an optional
  scope, so players pair again once for both; a token without it shows Pair again.
- **Screen off during downloads** ✅: while games download, the firmware turns the backlight off after
  60 s without a press instead of the player's 2, 5 or 10 minutes (a player's Off is left alone),
  and the press that lights it again does nothing else. Confirmed on a PSP-1000 (B5); see
  [Hardware findings](hardware-findings.md#backlight).

## Phase 4: Save sync ✅

PSP saves to and from RomM 5.4 through `/api/sync/negotiate`: each game's save folders travel as
one unit, byte for byte argosy-sigil's (checked against sigil in CI, not linked), in the `autosave`
slot where RomM clients such as Argosy keep PPSSPP's saves. Skiff keeps its own record of each sync
and never overwrites silently: a save changed on both sides waits for the player, as does one
deleted on this PSP or emptied in RomM, and every restore is staged, journalled and backs up the
saves it replaces. The launch check applies the rest by itself. RomM's in-browser player uploads no PSP saves (a game's
own save data), so it takes no part. See [Architecture](architecture.md#saves) and the
[guide](../guide/06-sync-saves.md).

Hardware run S1 passed on a PSP-1000 against RomM 5.4.0 (2026-10-12): a real PSP and PPSSPP 1.20.4
load each other's saves (one game, both ways), a sync cut by the Wi-Fi switch or by sleep leaves
both sides whole, and with the release EBOOT an 8 MB save hashes in 1.6 s and restores in
14–18 s. Sleep found four bugs, fixed in #99. See [Hardware findings](hardware-findings.md#save-sync-s1).

## Games sent from RomM ✅

Built (#103, #105, #108) for the next release.

A player picks the PSP in RomM 5.4's web UI (**Install on device**), on a phone or computer, and
Skiff adds the game to Downloads without asking. Skiff holds no socket, so it claims sends right
after connecting and then every 10 s while connected (RomM shows the PSP online for 90 s after a
claim); a send made while Skiff is closed waits in RomM, two days by default. Skiff reports each
send to RomM once it is queued ("Added to downloads"), already installed, or refused with its
reason in the PSP's language: a game of another platform (219), one Skiff cannot install, a
changed installed copy, or no room. A Settings switch (on by default) tells RomM to stop listing
the PSP. See [Architecture](architecture.md#the-app) and the
[guide](../guide/04-connect-to-romm.md#games-sent-from-romm).

Hardware run I1 passed on a PSP-1000 against RomM 5.4.0 (2026-10-12): the PSP shows online, sends
land on the next claim, also those made while Skiff was closed or the Wi-Fi was off, refusals reach
RomM with their reason, and an empty claim takes about 170 ms without holding up a download or
the screen; claims every 20 s were too slow to wait for, so #108 made it 10 s. See
[Hardware findings](hardware-findings.md#games-sent-from-romm-i1).

## Phase 5: PS1

Next. Decide how `.bin/.cue/.chd` becomes `EBOOT.PBP` (server-side conversion vs on-device
packaging), then add the PS1 installer. Server-side conversion now exists for PSP games (RomM 5.4
converts a CHD to CSO as Skiff downloads it, through rom-converto): a PS1 EBOOT would need
rom-converto to build one, which it does not yet. The format, the hardware tests behind it and the options
for converting are in [PS1 games on PSP](pops.md).

## Phase 6: More systems

Installers for emulated systems through PSP emulators, driven by
[platform requests](https://github.com/chiva/skiff/issues?q=label%3Aplatform).

## Also planned

- A documentation site (GitHub Pages) built from `docs/`.
- Proposing Skiff to the RomM project once it is usable, as other community clients have been.

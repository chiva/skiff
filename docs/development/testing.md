# Testing

Skiff is tested at three levels. The same self-test checks (`src/core/selftest.c`) run in all
three, so a failure on hardware that does not happen on the host points at the PSP environment, not
the logic.

| Tier | Where | In CI | Command |
|---|---|---|---|
| Unit | Host (Linux/macOS) | ✅ gcc + clang, plain + ASan/UBSan, coverage ≥85% | `scripts/dev.sh test asan coverage` |
| Emulator | PPSSPPHeadless | ✅ | `scripts/dev.sh psp selftest` |
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

`tests/emulator/run_selftest.sh` boots `skiff_selftest/EBOOT.PBP` in PPSSPPHeadless and passes only
when the output contains `SKIFF SELFTEST OK`. PPSSPPHeadless only shows a program's stdout inside its
full log (`-l`, lines starting `I stdout: `), so the script extracts those lines and prints the end
of the log when the marker is missing. The first local run builds the PPSSPP image, which
takes several minutes; later runs reuse it.

PPSSPP does not emulate the PSP's Wi-Fi hardware or real Memory Stick timing, so networking and
storage behaviour must also be checked on hardware.

## Hardware tier

Run before merging changes to `src/net/`, `src/storage/`, `src/platform/psp/`, or a toolchain
bump. Record the PSP model and firmware in the PR.

1. Set up PSPLINK ([debugging](debugging.md)) and build: `scripts/dev.sh psp`.
2. In `pspsh`: `./build/psp/skiff_selftest.prx` → expect `SKIFF SELFTEST OK`.
3. Run the app: `./build/psp/skiff.prx`, check the version on screen, press START, and
   check it returns cleanly.
4. Copy `build/psp/pbp/skiff/EBOOT.PBP` to `PSP/GAME/Skiff/` and launch it from the XMB; HOME →
   Quit must exit without freezing.

From the networking release on, the hardware tier adds: joining the TKIP test SSID; an HTTPS
request to a test RomM; TLS 1.2 with ECDHE confirmed in a packet capture; a proxy rejecting a
missing or wrong client certificate; a download interrupted (Wi-Fi switch off) and resumed; and
installing a homebrew ISO that then boots from ARK-5.

## Security probe

`scripts/dev.sh entropy-probe` builds and runs `tests/security/entropy_probe.c` in PPSSPPHeadless. It
calls the SDK's `getentropy()` twice and reports whether the bytes are identical, demonstrating the
weak entropy source Skiff must replace (see [Architecture](architecture.md#randomness-for-tls)). It
currently reports `DETERMINISTIC`; once Skiff registers its own entropy pool it must report
`VARYING`, at which point this becomes a regression test.

## Test data rules

Homebrew, public domain or synthetic only. No commercial games, BIOS files, real saves, tokens or
keys, ever.

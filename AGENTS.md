# Skiff: notes for agents and contributors

Skiff is a RomM client that runs on real PSP hardware (C11, pspdev toolchain). Read
`CONTRIBUTING.md` for the human workflow; this file holds the facts an agent needs to work safely.

## Pushing

Never push or open a PR unless the maintainer explicitly asks for it.

## Commands

All builds and checks run in containers. Docker is the only prerequisite.

| Task | Command |
|---|---|
| Host unit tests (gcc + clang) | `scripts/dev.sh test` → `build/host-{gcc,clang}/` |
| ASan + UBSan (gcc + clang) | `scripts/dev.sh asan` |
| Coverage (85% floor) | `scripts/dev.sh coverage` |
| clang-tidy + cppcheck | `scripts/dev.sh lint` |
| PSP EBOOTs | `scripts/dev.sh psp` → `build/psp/pbp/{skiff,skiff_selftest,skiff_tls_probe}/EBOOT.PBP` |
| Emulator self-test | `scripts/dev.sh selftest` (after `psp`) |
| TLS toolchain probe | `scripts/dev.sh tls-probe` (after `psp`) |
| Release zip | `scripts/dev.sh package` → `dist/` |
| Entropy probe | `scripts/dev.sh entropy-probe` (after `psp`) |

`scripts/dev.sh` accepts several commands: `scripts/dev.sh test asan lint psp selftest tls-probe`.
CI runs these same commands, so the compiler matrix lives only in `HOST_COMPILERS` in `dev.sh`.
PSP builds use the `skiff-toolchain` image (`docker/toolchain.Dockerfile`): pspdev, pinned as
`tag@digest`, plus Mbed TLS 4.1 and curl 8.22 pinned by SHA256. CI job names are required status
checks in the `main` ruleset: add steps or jobs, never rename existing ones.

## Layout rules

- `include/skiff/` public headers; `src/core/` and every other `src/` layer except `src/platform/`
  must compile and be unit-tested on the host.
- `src/platform/psp/` is the only place allowed to include `psp*.h` or call `sce*`.
- Errors: return `skiff_err` from `include/skiff/error.h`. Add codes to `SKIFF_ERROR_TABLE` at the end
  of their group; never renumber (codes appear in user bug reports).
- The self-test (`src/core/selftest.c`) runs on host, emulator and hardware. Each new layer adds
  its checks there.

## PSP toolchain gotchas

- pspdev images are amd64 only; on Apple Silicon they run under emulation (slow but correct).
- The toolchain adds SDK headers with `-I`; `cmake/SkiffPsp.cmake` re-adds them with `-isystem` so
  `-Wpedantic -Werror` applies only to our code.
- `module_info.c` must be linked as an OBJECT library. In a static archive nothing references it
  and the linker drops it ("no sceModuleInfo section found").
- `create_pbp_file` writes to `build/<preset>/pbp/<target>/`; its OUTPUT_DIR must not equal the
  executable's path.
- PARAM.SFO versions are `XX.YY`; the patch number is shown in-app only.
- `create_pbp_file` defaults to `MEMSIZE=2` (limited memory); `cmake/SkiffPsp.cmake` passes
  `MEMSIZE 1` so 64 MB models get their full RAM.
- PPSSPPHeadless prints a program's stdout only in its full log (`-l`, `I stdout: ` prefix).
- Every package linked into the EBOOT goes in `scripts/psp-packages.txt` so its licence ships
  (`mbedtls` and `curl` too, once the app links them: the toolchain image installs their licences
  where `psp-create-license-directory` looks).
- Mbed TLS's config lives in its installed headers (`docker/toolchain/configure-mbedtls.sh`); never
  pass `MBEDTLS_*CONFIG_FILE` defines to a consumer, or Skiff and libcurl disagree on struct layouts.
- An EBOOT linking Mbed TLS must provide `mbedtls_platform_get_entropy()` and `mbedtls_ms_time()`
  (link-time contracts, see `docs/development/toolchain.md`).
- "Clock skew detected" warnings from make in containers come from the Docker VM's clock and are
  harmless.

## Security invariants

- Never use the SDK's default TLS randomness: `_getentropy()` in pspsdk's `libcglue/glue.c`
  reseeds a Mersenne Twister with `time(NULL)` on every call. TLS entropy comes only from Skiff's
  `mbedtls_platform_get_entropy()`, which must return full entropy or
  `PSA_ERROR_INSUFFICIENT_ENTROPY` (never a weaker credit), and connections then fail with
  `SKIFF_ERR_NET_ENTROPY`. Never re-enable `MBEDTLS_PSA_BUILTIN_GET_ENTROPY` or
  `MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG`, and never implement the hook with `getentropy()`, `rand()` or
  the clock. Only test binaries may stub it, and only with a stub that refuses.
- Never log tokens, keys or full request headers.
- Paths built from RomM data must be sanitised before touching the Memory Stick.

## Tests and data

Fixtures must be homebrew, public domain or synthetic. Never add commercial ROMs, BIOS files,
real saves, or credentials. Hardware-tier steps are in `docs/development/testing.md`.

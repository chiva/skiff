# Skiff: notes for agents and contributors

Skiff is a RomM client that runs on real PSP hardware (C11, pspdev toolchain). Read
`CONTRIBUTING.md` for the human workflow; this file holds the facts an agent needs to work safely.

## Pushing

Never push or open a PR unless the maintainer explicitly asks for it.

## Commands

All builds and checks run in containers. Docker is the only prerequisite.

| Task | Command |
|---|---|
| Host unit tests | `scripts/dev.sh test` |
| ASan + UBSan | `scripts/dev.sh asan` |
| Coverage (85% floor) | `scripts/dev.sh coverage` |
| clang-tidy + cppcheck | `scripts/dev.sh lint` |
| PSP EBOOTs | `scripts/dev.sh psp` → `build/psp/pbp/{skiff,skiff_selftest}/EBOOT.PBP` |
| Emulator self-test | `scripts/dev.sh selftest` (after `psp`) |
| Release zip | `scripts/dev.sh package` → `dist/` |
| Entropy probe | `scripts/dev.sh entropy-probe` (after `psp`) |

`scripts/dev.sh` accepts several commands: `scripts/dev.sh test asan lint psp selftest`.

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
- Every package linked into the EBOOT goes in `scripts/psp-packages.txt` so its licence ships.
- "Clock skew detected" warnings from make in containers come from the Docker VM's clock and are
  harmless.

## Security invariants

- Never use the SDK's default TLS randomness: `_getentropy()` in pspsdk's `libcglue/glue.c`
  reseeds a Mersenne Twister with `time(NULL)` on every call. Networking code must register
  Skiff's own entropy source and fail with `SKIFF_ERR_NET_ENTROPY`.
- Never log tokens, keys or full request headers.
- Paths built from RomM data must be sanitised before touching the Memory Stick.

## Tests and data

Fixtures must be homebrew, public domain or synthetic. Never add commercial ROMs, BIOS files,
real saves, or credentials. Hardware-tier steps are in `docs/development/testing.md`.

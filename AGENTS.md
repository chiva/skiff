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
| PSP EBOOTs | `scripts/dev.sh psp` → `build/psp/pbp/{skiff,skiff_selftest,skiff_tls_probe,skiff_kirk_probe,skiff_ui_proto,skiff_net_probe}/EBOOT.PBP` |
| Emulator self-test | `scripts/dev.sh selftest` (after `psp`) |
| TLS toolchain probe | `scripts/dev.sh tls-probe` (after `psp`) |
| KIRK probe without ARK (TLS must refuse) | `scripts/dev.sh kirk-probe` (after `psp`) |
| UI prototype, headless (fonts and frames) | `scripts/dev.sh ui-proto` (after `psp`) |
| Network probe without ARK (modules load, TLS must refuse) | `scripts/dev.sh net-probe` (after `psp`) |
| Test RomM behind TLS/mTLS (Docker Compose) | `scripts/dev.sh romm-up` (or `romm-lan` for a PSP), `romm-check`, `romm-down` → `build/integration/` |
| Release zip | `scripts/dev.sh package` → `dist/` |
| Icon PNGs from `assets/brand/` SVGs | `scripts/dev.sh icons` → `assets/{psp,github}/` (commit them) |
| Hardware tier without PSPLINK | `scripts/memstick.sh install\|results\|uninstall <mount>` (host only, no Docker) |

`scripts/dev.sh` accepts several commands: `scripts/dev.sh test asan lint psp selftest tls-probe kirk-probe`.
CI runs these same commands, so the compiler matrix lives only in `HOST_COMPILERS` in `dev.sh`.
PSP builds use the `skiff-toolchain` image (`docker/toolchain.Dockerfile`): pspdev, pinned as
`tag@digest`, plus Mbed TLS 4.1 and curl 8.22 pinned by SHA256. CI job names are required status
checks in the `main` ruleset: add steps or jobs, never rename existing ones.

## Layout rules

- `include/skiff/` public headers; `src/core/` and every other `src/` layer except `src/platform/`
  must compile and be unit-tested on the host.
- `src/platform/psp/` is the only place allowed to include `psp*.h` or call `sce*`. The network
  stack (modules, access point, teardown) lives in `src/platform/psp/net_psp.c`; unload it only
  after a confirmed disconnect.
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
- Check EBOOTs report through `src/platform/psp/report.h` (stdout, screen, and `result.txt` next to
  the EBOOT, found from `argv[0]`); a new check EBOOT should use it too.
- Every package linked into the EBOOT goes in `scripts/psp-packages.txt` so its licence ships
  (`mbedtls` and `curl` too, once the app links them: the toolchain image installs their licences
  where `psp-create-license-directory` looks).
- Mbed TLS's config lives in its installed headers (`docker/toolchain/configure-mbedtls.sh`); never
  pass `MBEDTLS_*CONFIG_FILE` defines to a consumer, or Skiff and libcurl disagree on struct layouts.
- psp-gcc links `psputility`, `psprtc`, `pspnet_inet` and `pspnet_resolver` after everything else.
  Do not list them in `target_link_libraries`: a stub library linked twice splits its import stubs
  and psp-fixup-imports warns "stubs out of order" (the EBOOT may then not run).
- intraFont turns `GU_DEPTH_TEST` back on after every print. Geometry drawn after text must disable
  it first, or real hardware discards it against the uncleared depth buffer while PPSSPP draws it
  (seen in the UI prototype: invisible highlight and dialog backdrop). Write vertices from
  `sceGuGetMemory` back from the data cache (`sceKernelDcacheWritebackRange`) before drawing, as
  intraFont does.
- An EBOOT linking Mbed TLS must provide `mbedtls_platform_get_entropy()` and `mbedtls_ms_time()`
  (link-time contracts, see `docs/development/toolchain.md`), and link `skiff_psp_tls`, whose
  constructor gives Mbed TLS the real-time clock for certificate dates.
- `time()` on the PSP returns only the time of day (the date is lost). Never use it for anything
  date-related: read `sceRtcGetCurrentTick` (UTC) instead.
- ARK custom firmware functions are imported through hand-written stubs in
  `src/platform/psp/ark_sysctrl.S` (pspsdk ships none). A NID is the first four bytes of SHA-1 of the
  function name, read little-endian; an import from a library that is not loaded returns
  `0x8002013A`, so check `sctrlHENGetVersion()` before trusting other ARK calls.
- `scripts/dev.sh` works from git worktrees (it mounts the main repository's `.git` read-only in the
  containers). Parallel sessions should each work in their own worktree, never switch branches in
  a shared checkout.
- "Clock skew detected" warnings from make in containers come from the Docker VM's clock and are
  harmless.

## Security invariants

- TLS entropy comes only from Skiff's `mbedtls_platform_get_entropy()`, which must return full
  entropy or `PSA_ERROR_INSUFFICIENT_ENTROPY` (never a weaker credit), and connections then fail
  with the reason from `skiff_psp_entropy_status()`: `SKIFF_ERR_NET_NEEDS_ARK` or
  `SKIFF_ERR_NET_ENTROPY`. Never re-enable `MBEDTLS_PSA_BUILTIN_GET_ENTROPY` or
  `MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG`, and never implement the hook with `getentropy()`, `rand()` or
  the clock. Only test binaries may stub it, and only with a stub that refuses. The real hook is
  `src/platform/psp/kirk_entropy.c` (KIRK through ARK); it has no fallback, so without ARK
  networking refuses to start.
- Never log tokens, keys or full request headers.
- Paths built from RomM data must be sanitised before touching the Memory Stick.

## Tests and data

Fixtures must be homebrew, public domain or synthetic. Never add commercial ROMs, BIOS files,
real saves, or credentials. Hardware-tier steps are in `docs/development/testing.md`.

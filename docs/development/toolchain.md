# Toolchain

## In short

```bash
scripts/dev.sh psp          # debug EBOOTs in build/psp/pbp/
scripts/dev.sh psp-release  # release EBOOTs in build/psp-release/pbp/
scripts/dev.sh test         # host unit tests
```

Docker is the only requirement. `scripts/dev.sh help` lists every command.

## PSP: pspdev

- Image: `pspdev/pspdev:v20261001` (GCC 15.2, newlib 4.5, CMake 4.2). Monthly tags; Renovate
  proposes updates in a dedicated PR so the hardware tier can be run before merging.
- **amd64 only.** On Apple Silicon Docker runs it under emulation: slower, but the output is
  identical.
- CMake uses pspdev's toolchain file (`$PSPDEV/psp/share/pspdev.cmake`) through the `psp` and
  `psp-release` presets. `create_pbp_file()` (from pspdev's `CreatePBP.cmake`) packs each
  executable into an `EBOOT.PBP`.

Building natively, without Docker, works too: install pspdev from its
[releases](https://github.com/pspdev/pspdev/releases), set `PSPDEV`, put `$PSPDEV/bin` on
`PATH`, then `cmake --preset psp && cmake --build --preset psp`.

## Host

`docker/host.Dockerfile` (Ubuntu 24.04 with cmake, gcc, clang-tidy, cppcheck, gcovr) matches the CI
runners. Natively on macOS: `brew install cmake` and use Apple clang with the `host` and `host-asan`
presets.

## Presets

| Preset | What |
|---|---|
| `host` | Debug unit tests |
| `host-asan` | Unit tests under AddressSanitizer + UndefinedBehaviorSanitizer |
| `host-coverage` | Unit tests with gcov; `gcovr` enforces 85% lines over portable code |
| `psp` | Debug EBOOTs (not stripped, good for `psp-gdb`) |
| `psp-release` | Release EBOOTs (stripped) |

All presets build with `-Werror` and a strict warning set (`cmake/SkiffWarnings.cmake`).

## Dependencies

| Dependency | Where | Pinned by |
|---|---|---|
| pspdev toolchain | Docker image | tag, Renovate |
| Unity (C test framework) | `tests/CMakeLists.txt` FetchContent | commit, Renovate |
| PPSSPP (emulator tests) | `docker/ppsspp.Dockerfile` | commit, Renovate |
| GitHub Actions | workflows | commit SHA, Renovate |
| pre-commit hooks | `.pre-commit-config.yaml` | commit SHA, Renovate |

Planned (Phase 1): a Skiff toolchain image on top of `pspdev/pspdev` with pinned curl 8.x and
mbedtls 3.6 LTS, because pspdev's packages are too old to ship (curl 7.64.1, mbedtls 2.28). cJSON
and intraFont come from pspdev's packages; argosy-sigil will be a pinned submodule.

## Licences

Every package linked into the EBOOT must be listed in `scripts/psp-packages.txt`. `scripts/dev.sh
package` and the release workflow then collect its licence (and its dependencies') with pspdev's
`psp-create-license-directory` into the zip's `third-party-licenses/`. pspsdk, newlib and
pthread-embedded are always included; see [Architecture](architecture.md#licensing-of-the-binary)
for what that implies.

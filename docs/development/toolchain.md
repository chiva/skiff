# Toolchain

## In short

```bash
scripts/dev.sh psp          # debug EBOOTs in build/psp/pbp/
scripts/dev.sh psp-release  # release EBOOTs in build/psp-release/pbp/
scripts/dev.sh test         # host unit tests
```

Docker is the only requirement. `scripts/dev.sh help` lists every command.

## PSP: pspdev

- Base image: `pspdev/pspdev:v20261001` (GCC 15.2, newlib 4.5, CMake 4.2), pinned by digest in
  `docker/toolchain.Dockerfile`, the only place it is referenced, so a re-pushed tag cannot change
  what we build or release with. Monthly tags; Renovate proposes tag and digest together in a
  dedicated PR so the hardware tier can be run before merging. The image build fails if the GCC
  major version changes.
- **amd64 only.** On Apple Silicon Docker runs it under emulation: slower, but the output is
  identical.
- CMake uses pspdev's toolchain file (`$PSPDEV/psp/share/pspdev.cmake`) through the `psp` and
  `psp-release` presets. `create_pbp_file()` (from pspdev's `CreatePBP.cmake`) packs each
  executable into an `EBOOT.PBP`.

Building natively, without Docker, works too: install pspdev from its
[releases](https://github.com/pspdev/pspdev/releases), set `PSPDEV`, put `$PSPDEV/bin` on
`PATH`, run `docker/toolchain/build-tls.sh "$PSPDEV/psp" -DCMAKE_TOOLCHAIN_FILE=$PSPDEV/psp/share/pspdev.cmake`
after removing pspdev's `curl`, `libzip` and `mbedtls` packages, then
`cmake --preset psp && cmake --build --preset psp`.

## The Skiff toolchain image

`docker/toolchain.Dockerfile` (`skiff-toolchain`) is pspdev with its TLS libraries replaced. Every PSP
build uses it: `scripts/dev.sh psp`, CI and the release job. It is built from source each time it
changes, not pulled from a registry. Why it replaces pspdev's TLS packages: [TLS on the PSP](tls.md).

| Library | Version | Why |
|---|---|---|
| Mbed TLS | 4.1.1 (LTS, supported until March 2029) | TLS 1.3; 3.6 LTS ends March 2027 |
| curl | 8.22.0 | HTTP/HTTPS only, IPv4, no optional dependencies |

pspdev's `curl`, `mbedtls` and `libzip` (the only other package depending on pspdev's mbedtls) are
removed first so no 2.28 header or archive can be picked up. Neither library needs a PSP patch.

**One build script, two images.** `docker/toolchain/build-tls.sh` holds the versions, checksums,
the Mbed TLS profile step and curl's options. The toolchain image runs it with pspdev's CMake
toolchain file; the host image (`docker/host.Dockerfile`) runs it natively into `/opt/skiff-tls`, so
host unit and integration tests link the same TLS code the EBOOTs do and see the same error codes
and certificate verify flags. The host's link-time contracts live in
`src/platform/host/tls_hooks.c` (entropy from the operating system's `getrandom()`, a monotonic
clock); they are never linked into an EBOOT.

**Mbed TLS profile.** `docker/toolchain/configure-mbedtls.sh` edits Mbed TLS's default config
headers in place with `sed`, so the installed headers carry the profile and Skiff, curl and mbedtls
agree on struct layouts. It removes the server side, DTLS, renegotiation, certificate writing,
persistent PSA keys, self-tests and debug strings. Every edit must change exactly one line and the
security-critical settings it does not edit are asserted, so an option renamed by an mbedtls
update fails the image build.

**No unpinned tools.** The image installs nothing from a package repository: the build uses only
what the digest-pinned pspdev image ships (cmake, make, tar, bzip2, sed), and curl is fetched as
`.tar.bz2` because the base image has no `xz`. The same commit therefore always builds with the
same tools.

**Link-time contracts.** The profile leaves two functions for the application to supply. Any EBOOT
that links Mbed TLS must provide both or it does not link:

| Function | Provided by | Purpose |
|---|---|---|
| `mbedtls_platform_get_entropy()` | `src/platform/psp/kirk_entropy.c` (KIRK through ARK) | the only seed for all TLS randomness; see [Architecture](architecture.md#randomness-for-tls) |
| `mbedtls_ms_time()` | `src/platform/psp/mbedtls_time.c` | monotonic milliseconds for TLS 1.3 ticket ages |

The same file installs the clock that certificate dates are checked against
(`MBEDTLS_PLATFORM_TIME_ALT`, set with `mbedtls_platform_set_time()` from a constructor, so it is in
place before `main()`): UTC seconds from the PSP's real-time clock (`sceRtcGetCurrentTick`). The C
library's `time()` cannot serve. On a PSP-1000 it returned 36834 at 09:13 UTC: only the time of day,
which made every certificate look issued in the future.

**Bumping a version.** Renovate opens a "TLS libraries" PR that fails the image's `sha256sum -c`
on purpose. Verify the new archive, then update its `*_SHA256` value in
`docker/toolchain/build-tls.sh`:

- Mbed TLS: compare with the SHA256 in the release notes
  (`gh release view mbedtls-<version> --repo Mbed-TLS/mbedtls`).
- curl: check the signature with Daniel Stenberg's key, fingerprint
  `27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2`
  (`gpgv --keyring <dearmored key> curl-<version>.tar.bz2.asc curl-<version>.tar.bz2`), then
  `sha256sum` the archive.

Mbed TLS stays on the 4.1 LTS line (Renovate's `allowedVersions`); moving to the next LTS is a
deliberate change.

## The CA bundle

The app trusts Mozilla's root certificates by default, as curl publishes them
([CA extract](https://curl.se/docs/caextract.html)). `docker/ca-bundle/fetch-ca-bundle.sh` fetches
one dated file (`cacert-<date>.pem`), checks it against the SHA256 pinned there and installs it,
with the MPL-2.0 text beside it, into the toolchain image at `$SKIFF_CA_BUNDLE_DIR`. It runs in a
layer after the TLS build, so a bundle bump does not recompile mbedtls and curl. Fetched with the
image like the TLS archives rather than committed, so the repository holds no third-party file and
every PSP build has the bundle without a network step of its own.

- `scripts/dev.sh psp` and `psp-release` copy it next to the app's EBOOT
  (`build/<preset>/pbp/skiff/cacert.pem`), where `scripts/memstick.sh install` and the release zip
  take it from.
- `scripts/collect-licenses.sh` adds its licence as `third-party-licenses/cacert/`.

**Bumping it.** Take the newest dated file from the CA extract page, compare its SHA256 with the
`cacert-<date>.pem.sha256` curl.se publishes beside it, and update `CA_BUNDLE_DATE` and
`CA_BUNDLE_SHA256` together. Renovate does not track it; bump it before a release when Mozilla's
list has changed.

## Host

`docker/host.Dockerfile` (Ubuntu 24.04 with cmake, gcc, clang, clang-tidy, cppcheck, gcovr) is the
only host environment: CI's host jobs run `scripts/dev.sh` too. `test` and `asan` build every
preset with each compiler in `HOST_COMPILERS` (gcc and clang) into `build/<preset>-<compiler>/`,
because clang flags warnings gcc does not and `-Werror` makes them fatal. Natively on macOS:
`brew install cmake` and use Apple clang with the `host` and `host-asan` presets.

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
| pspdev toolchain | `docker/toolchain.Dockerfile` `FROM` | tag + digest, Renovate |
| Mbed TLS, curl | `docker/toolchain.Dockerfile` | version + SHA256, Renovate + manual verification |
| Mozilla CA bundle (`cacert.pem`) | `docker/ca-bundle/fetch-ca-bundle.sh` | date + SHA256, manual (above) |
| QR Code generator (Nayuki, C, MIT) | `docker/qrcodegen/build-qrcodegen.sh`, built into the toolchain and host images | version + SHA256, Renovate + manual verification |
| Unity (C test framework) | `tests/CMakeLists.txt` FetchContent | commit, Renovate |
| PPSSPP (emulator tests) | `docker/ppsspp.Dockerfile` | commit, Renovate; Debian base by digest and packages from a dated snapshot.debian.org archive (bump `SNAPSHOT` with the commit); one-line patch for `flash0:` reads, checked at build time |
| GitHub Actions | workflows | commit SHA, Renovate |
| pre-commit hooks | `.pre-commit-config.yaml` | commit SHA, Renovate |

cJSON and intraFont come from pspdev's packages; argosy-sigil will be a pinned submodule.

## Licences

Every package linked into the EBOOT must be listed in `scripts/psp-packages.txt`. `scripts/dev.sh
package` and the release workflow then collect its licence (and its dependencies') with pspdev's
`psp-create-license-directory` into the zip's `third-party-licenses/`. pspsdk, newlib,
pthread-embedded and the CA bundle's MPL-2.0 are always included; see [Architecture](architecture.md#licensing-of-the-binary)
for what that implies.
